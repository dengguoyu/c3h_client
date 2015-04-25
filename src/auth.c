﻿/*
 * Filename:     auth.c
 *
 * Created by:	 liuqun
 * Revised by:   KiritoA
 * Description:  801.1X认证核心函数
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>
#include <time.h>
#include <stdbool.h>

#include <pcap.h>

#ifndef WIN32
#include <unistd.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#else

#endif

#include "auth.h"
#include "md5.h"
#include "defs.h"
#include "adapter.h"

// 子函数声明
static void HandleH3CRequest(int type, const uint8_t request[]);

static void SendEAPOLPacket(uint8_t type);
static void SendEAPPacket(uint8_t code, uint8_t type, uint8_t id, uint8_t *extPkt, uint16_t extLen);

static void SendStartPkt();
static void SendLogOffPkt();
static void SendResponseIdentity(const uint8_t request[]);
static void SendResponseIdencifyFake(const uint8_t request[]);
static void SendResponseMD5(const uint8_t request[]);
static void SendResponseSecurity(const uint8_t request[]);
static void SendResponseNotification(const uint8_t request[]);

static void FillClientVersionArea(uint8_t area[]);
//static void FillWindowsVersionArea(uint8_t area[]);
static void FillBase64Area(char area[]);
static void FillMD5Area(uint8_t digest[], uint8_t id, const char passwd[],
		const uint8_t srcMD5[]);

int got_packet(uint8_t *args, const struct pcap_pkthdr *header, const uint8_t *packet);

// typedef
typedef enum
{
	REQUEST = 1, RESPONSE = 2, SUCCESS = 3, FAILURE = 4, H3CDATA = 10
} EAP_Code;
typedef enum
{
	IDENTITY = 1, NOTIFICATION = 2, MD5_CHALLENGE = 4, SECURITY = 20
} EAP_Type;
typedef uint8_t EAP_ID;


const uint8_t BroadcastAddr[6] =
{ 0xff, 0xff, 0xff, 0xff, 0xff, 0xff }; // 广播MAC地址
const uint8_t MultcastAddr[6] =
{ 0x01, 0x80, 0xc2, 0x00, 0x00, 0x03 }; // 多播MAC地址
const char H3C_VERSION[16] = "EN\x11V7.00-0102"; // 华为客户端版本号
//const char H3C_KEY[64]    ="HuaWei3COM1X";  // H3C的固定密钥
const char H3C_KEY[64] = "Oly5D62FaE94W7"; // H3C的另一个固定密钥，网友取自MacOSX版本的iNode官方客户端

const int DefaultTimeout = 1500; //设置接收超时参数，单位ms
char errbuf[PCAP_ERRBUF_SIZE];
char FilterStr[100];
struct bpf_program fcode;

uint8_t local_ip[4] = { 0 };	// ip address
uint8_t local_mac[6];

eth_header_t eth_header; // ethernet header

/* 认证信息 */
const char *username = NULL;
const char *password = NULL;
const char *deviceName = NULL;

/* pcap */
pcap_t *adhandle = NULL; // adapter handle

bool isConnected = false;

void InitDevice(const char *DeviceName)
{
	// NOTE: 这里没有检查网线是否已插好,网线插口可能接触不良

	/* 打开适配器(网卡) */
	adhandle = pcap_open_live(DeviceName, 65536, 1, DefaultTimeout, errbuf);
	if (adhandle == NULL)
	{
		PRINTERR("%s\n", errbuf);
		exit(-1);
	}
	deviceName = DeviceName;
	/* 查询本机MAC地址 */
	GetMacFromDevice(local_mac, deviceName);
}

void CloseDevice()
{
	if (adhandle != NULL)
		pcap_close(adhandle);
}

bool flag = false;
uint32_t count = 0;

/**
 * 函数：Authentication()
 *
 * 使用以太网进行802.1X认证(802.1X Authentication)
 * 该函数将不断循环，应答802.1X认证会话，直到遇到错误后才退出
 */
int Authentication(const char *UserName, const char *Password)
{
	struct pcap_pkthdr *header = NULL;

	username = UserName;
 	password = Password;

	/*
	* 设置过滤器：
	* 初始情况下只捕获发往本机的802.1X认证会话，不接收多播信息（避免误捕获其他客户端发出的多播信息）
	* 进入循环体前可以重设过滤器，那时再开始接收多播信息
	*/
	sprintf(FilterStr,
		"(ether proto 0x888e) and (ether dst host %02x:%02x:%02x:%02x:%02x:%02x)",
		local_mac[0], local_mac[1], local_mac[2], local_mac[3], local_mac[4], local_mac[5]);
	pcap_compile(adhandle, &fcode, FilterStr, 1, 0xff);
	pcap_setfilter(adhandle, &fcode);

	// 初始化应答包的报头
	// 默认以多播方式应答802.1X认证设备发来的Request
	memcpy(eth_header.dest_mac, MultcastAddr, 6);
	memcpy(eth_header.src_mac, local_mac, 6);
	eth_header.eth_type = htons(0x888e);	//88 8e

	/* 主动发起认证会话 */
	SendStartPkt();
	PRINTMSG( "C3H Client: Connecting to the network ...\n");

	int retcode = 0;
	int retry = 0;

	const u_char *captured = NULL;

	/* 等待认证服务器的回应 */
	bool serverFound = false;
	while (!serverFound)
	{
		retcode = pcap_next_ex(adhandle, &header, &captured);
		if (retcode == 1 && (EAP_Code) captured[18] == REQUEST)
			serverFound = true;
		else
		{
			//重试达到最大次数后退出
			if (retry++ == 5)
			{
				PRINTERR("\n[ERROR] C3H Client: Server did not respond\n");
				return ERR_NOT_RESPOND;
			}
			// 延时后重试
			sleep(2);
			PRINTMSG(".");
			SendStartPkt();
			// NOTE: 这里没有检查网线是否接触不良或已被拔下
		}
	}

	//收到报文后修改为单播地址认证
	memcpy(eth_header.dest_mac, captured + 6, 6);

	// 重设过滤器，只捕获华为802.1X认证设备发来的包（包括多播Request Identity / Request AVAILABLE）
	sprintf(FilterStr,
			"(ether proto 0x888e) and (ether src host %02x:%02x:%02x:%02x:%02x:%02x)",
			captured[6], captured[7], captured[8], captured[9],
			captured[10], captured[11]);
	pcap_compile(adhandle, &fcode, FilterStr, 1, 0xff);
	pcap_setfilter(adhandle, &fcode);
		
	// 进入循环体
	for (;;)
	{
		// 调用pcap_next_ex()函数捕获数据包
		if (pcap_next_ex(adhandle, &header, &captured) == 1)
		{
			if ((retcode = got_packet(NULL, header, captured)) != 0)
				break;
		}
			
	}
	
	return (retcode);
}

void LogOff()
{
	if(isConnected)
	{
		SendLogOffPkt(adhandle, local_mac);

		PRINTMSG( "C3H Client: Log off.\n");
	}
	else
	{
		PRINTMSG( "C3H Client: Cancel.\n");
	}
	pcap_breakloop(adhandle);
}

int got_packet(uint8_t *args, const struct pcap_pkthdr *header, const uint8_t *packet)
{
	int retcode = 0;

	const eap_header_t *eapHeader = (eap_header_t*)(packet + ETH_LEN);

	uint8_t errtype = packet[22];
	uint8_t msgsize = packet[23];
	const char *msg = (const char*)&packet[24];

	switch (eapHeader->code)
	{
	case REQUEST:
		// 根据收到的Request，回复相应的Response包
		HandleH3CRequest(eapHeader->type, packet);
		if (flag)
		{
			//隔一段时间重新发起一次认证以保持不断线
			if (++count == 4)
			{
				count = 0;
				SendStartPkt();
			}
		}
		break;
	case SUCCESS:
		isConnected = true;
		PRINTMSG("C3H Client: You have passed the identity authentication\n");
		// 刷新IP地址
		PRINTMSG("C3H Client: Obtaining IP address...\n");
		RefreshIPAddress();
		//GetIpFromDevice(local_ip, deviceName);
		//PRINTMSG("C3H Client: Current IP address is %d.%d.%d.%d\n", local_ip[0], local_ip[1], local_ip[2], local_ip[3]);
		break;
	case FAILURE:
		isConnected = false;
		// 处理认证失败信息

		PRINTERR("[ERROR] C3H Client: Failure.\n");
		if (errtype == 0x09 && msgsize > 0)
		{	// 输出错误提示消息
			PRINTERR("%s\n", msg);
			// 已知的几种错误如下
			// E2531:用户名不存在
			// E2535:Service is paused
			// E2542:该用户帐号已经在别处登录
			// E2547:接入时段限制
			// E2553:密码错误
			// E2602:认证会话不存在
			// E3137:客户端版本号无效
			// fosu
			// E63100:客户端版本号无效
			// E63013:用户被列入黑名单
			if (strncmp(msg, "E63100", 6) == 0)
				return ERR_AUTH_INVALID_VERSION;
			else
				return ERR_AUTH_FAILED;
		}
		else if (errtype == 0x08) // 可能网络无流量时服务器结束此次802.1X认证会话
		{	// 遇此情况客户端立刻发起新的认证会话
			//goto START_AUTHENTICATION; 
			return ERR_UNKNOWN_FAILED;
		}
		else
		{
			PRINTERR("errtype=0x%02x\n", errtype);
			return ERR_UNKNOWN_FAILED;
		}

		retcode = errtype;
		break;
	case H3CDATA:
		PRINTMSG("[%d] Server: (H3C data)\n", eapHeader->id);
		// TODO: 需要解出华为自定义数据包内容，该部分内容与心跳包数据有关
		break;
	default:
		PRINTDEBUG("[%d] Server: Unknown EAP code:%d\n", eapHeader->id, eapHeader->code);
		break;
	}

	return retcode;
}

static void HandleH3CRequest(int type, const uint8_t request[])
{
	switch (type)
	{
	case IDENTITY:
		if (!isConnected)
			PRINTMSG("C3H Client: Beginning authentication... [%s]\n", username);
		else
			PRINTDEBUG("[%d] Server: Request Identity!\n", (EAP_ID)request[19]);

		if (!flag)
		{
			SendResponseIdentity(request);
			PRINTDEBUG("[%d] Client: Response Identity.\n", (EAP_ID)request[19]);
		}
		else
		{
			SendResponseIdencifyFake(request);
			PRINTDEBUG("[%d] Client: Response Identity*\n", (EAP_ID)request[19]);
		}

		break;
	case SECURITY:
		PRINTDEBUG("[%d] Server: Request Security!\n",
				(EAP_ID )request[19]);
		SendResponseSecurity(request);

		//jailbreak-test:从收到此心跳报文开始进入jailbreak模式
		if (!flag)
		{
			flag = true;
			SendStartPkt();
		}
		PRINTDEBUG("[%d] Client: Response Security.\n",
				(EAP_ID )request[19]);
		break;
	case MD5_CHALLENGE:
		if (!isConnected)
			PRINTMSG("C3H Client: Authenticating password...\n");
		else
			PRINTDEBUG("[%d] Server: Request MD5-Challenge!\n", (EAP_ID)request[19]);
		SendResponseMD5(request);
		PRINTDEBUG("[%d] Client: Response MD5-Challenge.\n",
				(EAP_ID )request[19]);
		break;
	case NOTIFICATION:
		if (!isConnected)
			PRINTMSG("C3H Client: Server responded\n");
		else
			PRINTDEBUG("[%d] Server: Request Notification!\n", request[19]);
		// 发送Response Notification
		SendResponseNotification(request);
		PRINTDEBUG("[%d] Client: Response Notification.\n", request[19]);
		break;
	default:
		PRINTDEBUG("[%d] Server: Request (type:%d)!\n", (EAP_ID)request[19], (EAP_Type)request[22]);
		PRINTDEBUG("Error! Unexpected request type\n");
		break;
	}
}

static void SendEAPPacket(uint8_t code, uint8_t type, uint8_t id, uint8_t *extPkt, uint16_t extLen)
{
	uint8_t packet[120];
	size_t i = 0;
	eap_header_t eap_header; // eap header

	// Fill Ethernet header
	memcpy(packet, &eth_header, ETH_LEN);
	i += ETH_LEN;
	// 802.1X Authentication
	eap_header.header.type = 0x00;	//Type: EAP Packet (0)
	eap_header.header.version = 0x01;	//Version: 802.1X-2001 (1)
	eap_header.header.length = htons(extLen + EAP_HDR_LEN);
	// Extensible Authentication Protocol
	eap_header.code = code;
	eap_header.id = id;
	eap_header.length = htons(extLen + EAP_HDR_LEN);
	eap_header.type = type;
	memcpy(packet + i, &eap_header, EAP_HDR_LEN + EAPOL_HDR_LEN);
	i += (EAPOL_HDR_LEN + EAP_HDR_LEN);

	memcpy(packet + i, extPkt, extLen);
	i += extLen;
	
	if (i < EAP_MIN_LEN)
	{
		memset(packet + i, 0x00, 60 - i);
		i = EAP_MIN_LEN;
	}
	// 发包
	pcap_sendpacket(adhandle, packet, i);
}

static void SendEAPOLPacket(uint8_t type)
{
	uint8_t packet[64];
	eapol_header_t eapol_header; // eapol header

	// Fill Ethernet header
	memcpy(packet, &eth_header, ETH_LEN);

	// EAPOL header
	eapol_header.version = 0x01;	//Version: 802.1X-2001 (1)
	eapol_header.type = type;
	eapol_header.length = 0x00;

	memcpy(packet + 14, &eapol_header, EAPOL_HDR_LEN);

	memset(packet + 18, 0x00, (64 - ETH_LEN - EAPOL_HDR_LEN));	//剩余字节填充0

	if (flag)
	{
		memcpy(packet, MultcastAddr, 6);
	}

	// 发包
	pcap_sendpacket(adhandle, packet, sizeof(packet));
}

static void SendStartPkt()
{
	SendEAPOLPacket(0x01);	// Type=Start
}

static void SendLogOffPkt()
{
	SendEAPOLPacket(0x02);	// Type=Logoff
}

static void SendResponseNotification(const uint8_t request[])
{
	uint8_t response[50];

	assert((EAP_Code )request[18] == REQUEST);
	assert((EAP_Type )request[22] == NOTIFICATION);

	// Extensible Authentication Protocol
	size_t i = 0;
	/* Notification Data */
	// 其中前2+20字节为客户端版本
	response[i++] = 0x01; // type 0x01
	response[i++] = 0x16;   // lenth
	FillClientVersionArea(response + i);
	i += 20;

	//2015.4.13 佛大客户端修订,不需要系统版本号
	//最后2+20字节存储加密后的Windows操作系统版本号
	/*
		response[i++] = 0x02; // type 0x02
		response[i++] = 22;   // length
		FillWindowsVersionArea(response+i);
		i += 20;*/
	// }

	SendEAPPacket((EAP_Code)RESPONSE, (EAP_Type)NOTIFICATION, request[19], response, i);
}


static void SendResponseSecurity(const uint8_t request[])
{
	size_t i = 0;
	size_t usernamelen;
	uint8_t response[100];

	assert((EAP_Code )request[18] == REQUEST);
	assert((EAP_Type )request[22] == SECURITY);

	// Extensible Authentication Protocol
	// Type-Data
	//response[i++] = 0x00;	// 上报是否使用代理，取消此处注释会导致马上断线拉黑
	//暂时未能解密该部分内容，只作填充0处理
	/*
	response[i++] = 0x16;
	response[i++] = 0x20;	//Length
	//memcpy(response + i, pulse, 32);
	memset(response + i, 0x00, 32);
	i += 32;
				
	GetIpFromDevice(local_ip, deviceName);
	response[i++] = 0x15;	  // 上传IP地址
	response[i++] = 0x04;	  //
	memcpy(response + i, local_ip, 4);	  //
	i += 4;			  //
	*/
	response[i++] = 0x06;		  // 携带版本号
	response[i++] = 0x07;		  //
	FillBase64Area((char*) response + i);		  //
	i += 28;			  //
	response[i++] = ' '; // 两个空格符
	response[i++] = ' '; //
	usernamelen = strlen(username);
	memcpy(response + i, username, usernamelen); //
	i += usernamelen;			  //
	// }

	SendEAPPacket((EAP_Code)RESPONSE, (EAP_Type)SECURITY, request[19], response, i);
}

static void SendResponseIdentity(const uint8_t request[])
{
	uint8_t response[100];
	size_t i = 0;
	size_t usernamelen;

	assert((EAP_Code )request[18] == REQUEST);
	assert((EAP_Type )request[22] == IDENTITY);

	// Extensible Authentication Protocol
	if(isConnected)
	{
		//连接后需要上报的内容
		//TODO:暂时未能解密该部分内容，只作填充0处理
		/*
		response[i++] = 0x16;
		response[i++] = 0x20;	//Length
		//memcpy(response + i, pulse, 32);
		memset(response+i, 0x00, 32);
		i += 32;
				
		GetIpFromDevice(local_ip, deviceName);
		response[i++] = 0x15;	  // 上传IP地址
		response[i++] = 0x04;	  //
		memcpy(response+i, local_ip, 4);//
		i += 4;*/
	}

	response[i++] = 0x06;		  // 携带版本号
	response[i++] = 0x07;		  //
	FillBase64Area((char*) response + i);		  //
	i += 28;			  //
	response[i++] = ' '; // 两个空格符
	response[i++] = ' '; //
	usernamelen = strlen(username); //末尾添加用户名
	memcpy(response + i, username, usernamelen);
	i += usernamelen;
	assert(i <= sizeof(response));

	SendEAPPacket((EAP_Code)RESPONSE, (EAP_Type)IDENTITY, request[19], response, i);
}

//发送Header正确内容无效的心跳报文，令服务器直接忽略该报文数据
static void SendResponseIdencifyFake(const uint8_t request[])
{
	uint8_t response[128];
	size_t i;
	uint16_t eaplen;
	size_t usernamelen;

	assert((EAP_Code)request[18] == REQUEST);

	// Fill Ethernet header
	memcpy(response, &eth_header, ETH_LEN);

	// 802,1X Authentication
	response[14] = 0x01;	// 802.1X Version 1
	response[15] = 0x00;	// Type=0 (EAP Packet)
	//response[16~17]留空	// Length

	// Extensible Authentication Protocol
	response[18] = (EAP_Code)RESPONSE;	// Code
	response[19] = request[19];		// ID
	//response[20~21]留空			// Length
	response[22] = (EAP_Type)IDENTITY;	// Type
	// Type-Data
	i = 23;

	response[i++] = '\\';
	usernamelen = strlen(username); //末尾添加用户名
	memset(response + i, 0x00, usernamelen);
	//memcpy(response + i, username, usernamelen);
	i += usernamelen;

	assert(i <= sizeof(response));

	// 补填前面留空的两处Length
	eaplen = htons(0x3E);
	memcpy(response + 16, &eaplen, sizeof(eaplen));
	eaplen = htons(0x10);
	memcpy(response + 20, &eaplen, sizeof(eaplen));

	// 发送
	pcap_sendpacket(adhandle, response, i);
}

static void SendResponseMD5(const uint8_t request[])
{
	size_t usernamelen;
	uint8_t response[40];
	size_t i = 0;
	assert((EAP_Code )request[18] == REQUEST);
	assert((EAP_Type )request[22] == MD5_CHALLENGE);

	usernamelen = strlen(username);

	// Extensible Authentication Protocol
	response[i++] = 16;		// Value-Size: 16 Bytes
	FillMD5Area(response + i, request[19], password, request + 24);
	i += 16;
	memcpy(response + i, username, usernamelen);
	i += usernamelen;
	assert(i <= sizeof(response));

	SendEAPPacket((EAP_Code)RESPONSE, (EAP_Type)MD5_CHALLENGE, request[19], response, i);
}

// 函数: XOR(data[], datalen, key[], keylen)
//
// 使用密钥key[]对数据data[]进行异或加密
//（注：该函数也可反向用于解密）
static void XOR(uint8_t data[], unsigned dlen, const char key[], unsigned klen)
{
	unsigned int i, j;

	// 先按正序处理一遍
	for (i = 0; i < dlen; i++)
		data[i] ^= key[i % klen];
	// 再按倒序处理第二遍
	for (i = dlen - 1, j = 0; j < dlen; i--, j++)
		data[i] ^= key[j % klen];
}

static void FillClientVersionArea(uint8_t area[20])
{
	uint32_t random;
	char RandomKey[8 + 1];

	random = (uint32_t) time(NULL);    // 注：可以选任意32位整数
	sprintf(RandomKey, "%08x", random);    // 生成RandomKey[]字符串

	// 第一轮异或运算，以RandomKey为密钥加密16字节
	memcpy(area, H3C_VERSION, sizeof(H3C_VERSION));
	XOR(area, 16, RandomKey, strlen(RandomKey));

	// 此16字节加上4字节的random，组成总计20字节
	random = htonl(random); // （需调整为网络字节序）
	memcpy(area + 16, &random, 4);

	// 第二轮异或运算，以H3C_KEY为密钥加密前面生成的20字节
	XOR(area, 20, H3C_KEY, strlen(H3C_KEY));
}

/*
static
void FillWindowsVersionArea(uint8_t area[20])
{
	const uint8_t WinVersion[20] = "r70393861";

	memcpy(area, WinVersion, 20);
	XOR(area, 20, H3C_KEY, strlen(H3C_KEY));
}*/

static void FillBase64Area(char area[])
{
	uint8_t version[20];
	const char Tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
			"abcdefghijklmnopqrstuvwxyz"
			"0123456789+/"; // 标准的Base64字符映射表
	uint8_t c1, c2, c3;
	int i, j;

	// 首先生成20字节加密过的H3C版本号信息
	FillClientVersionArea(version);

	// 然后按照Base64编码法将前面生成的20字节数据转换为28字节ASCII字符
	i = 0;
	j = 0;
	while (j < 24)
	{
		c1 = version[i++];
		c2 = version[i++];
		c3 = version[i++];
		area[j++] = Tbl[(c1 & 0xfc) >> 2];
		area[j++] = Tbl[((c1 & 0x03) << 4) | ((c2 & 0xf0) >> 4)];
		area[j++] = Tbl[((c2 & 0x0f) << 2) | ((c3 & 0xc0) >> 6)];
		area[j++] = Tbl[c3 & 0x3f];
	}
	c1 = version[i++];
	c2 = version[i++];
	area[24] = Tbl[(c1 & 0xfc) >> 2];
	area[25] = Tbl[((c1 & 0x03) << 4) | ((c2 & 0xf0) >> 4)];
	area[26] = Tbl[((c2 & 0x0f) << 2)];
	area[27] = '=';
}


static void FillMD5Area(uint8_t digest[], uint8_t id, const char passwd[], const uint8_t srcMD5[])
{
	uint8_t	msgbuf[128]; // msgbuf = ‘id‘ + ‘passwd’ + ‘srcMD5’
	size_t	msglen;
	size_t	passlen;

	passlen = strlen(passwd);
	msglen = 1 + passlen + 16;
	assert(sizeof(msgbuf) >= msglen);

	msgbuf[0] = id;
	memcpy(msgbuf+1,	 passwd, passlen);
	memcpy(msgbuf+1+passlen, srcMD5, 16);

	md5_state_t state;
	md5_init(&state);
	md5_append(&state, (const md5_byte_t *)msgbuf, msglen);
	md5_finish(&state, digest);
}
