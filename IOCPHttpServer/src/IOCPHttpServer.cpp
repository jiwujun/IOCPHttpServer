#include "IOCPHttpServer.h"
#include <iostream>
#include<WS2tcpip.h>

// 每个处理器上生成的线程数量（建议为2 或 1）
#define WORKER_THREADS_PER_POCESSOR 2

// 同时投递的 Accept 请求的数量
#define MAX_POST_ACCEPT 10

// 传递给 Worker 线程的退出信号
#define EXIT_CODE NULL


// 释放指针宏
#define RELEASE(x)	{if(x != NULL){delete x; x = NULL;}}
// 释放 Socket 宏
#define RELEASE_SOCKET(x)	{if(x != INVALID_SOCKET) { closesocket(x); x=INVALID_SOCKET;}}
// 释放句柄 宏
#define RELEASE_HANDLE(x)	{if(x != NULL && x != INVALID_HANDLE_VALUE) { CloseHandle(x); x = NULL; }}


IOCPServer::IOCPServer() :
	m_nThreads(0),
	m_hShutdownEvent(NULL),
	m_hIOCompletionPort(NULL),
	m_phWorkerThreads(NULL),
	m_nPort(SERVER_PORT),
	m_lpfuAcceptEx(NULL),
	m_lpfnGetAcceptExSockAddrs(NULL),
	m_pServerContext(NULL)
{

}


IOCPServer::~IOCPServer()
{
	Stop();
}


bool IOCPServer::Start()
{
	// 初始化线程互斥量
	InitializeCriticalSection(&m_csContextList);

	// 初始化系统退出事件
	m_hShutdownEvent = CreateEvent(NULL, TRUE, FALSE, NULL);

	// 初始化完成端口
	if (!_InitIOCP())
	{
		std::cout << "初始化完成端口失败\n";
		return false;
	}
	std::cout << "初始化完成端口完成!\n";

	if (!_InitServerSocket())
	{
		return false;
	}

	std::cout << "初始成功！正在等待客户端连接...\n";
	return true;
}

void IOCPServer::Stop()
{
	if (m_pServerContext != NULL && m_pServerContext->m_Socket != INVALID_SOCKET)
	{
		// 激活关闭消息的事件通知
		SetEvent(m_hShutdownEvent);

		for (int i = 0; i < m_nThreads; i++)
		{
			PostQueuedCompletionStatus(m_hIOCompletionPort, 0, (DWORD)EXIT_CODE, NULL);
		}
		// 等待所有工作者线程关闭
		WaitForMultipleObjects(m_nThreads, m_phWorkerThreads, TRUE, INFINITE);

		// 清除客户端上下文列表
		_ClearContextList();

		//释放所有资源
		_DeInit();
		// 关闭 socket 服务
		WSACleanup();
		std::cout << "服务器已关闭！\n";
	}
}


/*
*初始化完成端口，并且建立 工作者线程
*/
bool IOCPServer::_InitIOCP()
{
	// 新建 完成端口
	m_hIOCompletionPort = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
	if (!m_hIOCompletionPort)
	{
		std::cout << "建立完成端口失败！错误代码：" << WSAGetLastError() << std::endl;
		return false;
	}

	// 根据本机的处理器数量，建立对应的线程数
	m_nThreads = WORKER_THREADS_PER_POCESSOR * _GetNumOfProcessors();

	// 初始化工作者线程数组	(用完记得 delete)
	m_phWorkerThreads = new HANDLE[m_nThreads];

	// 建立工作者线程
	DWORD nThreadID;
	for (int i = 0; i < m_nThreads; i++)
	{
		THREADPARAMS_WORKER* pThreadParams = new THREADPARAMS_WORKER;
		pThreadParams->pIOCPModel = this;
		pThreadParams->nThreadNo = i + 1;
		m_phWorkerThreads[i] = CreateThread(0, 0, _WorkerThread, (void *)pThreadParams, 0, &nThreadID);
	}

	std::cout << "建立了 " << m_nThreads << " 个 工作者线程" << std::endl;
	return true;

}


/*
*获取本机的处理器数量
*返回值：
*	本机的处理器个数
*/
int IOCPServer::_GetNumOfProcessors()
{
	SYSTEM_INFO si;
	GetSystemInfo(&si);
	return si.dwNumberOfProcessors;
}


/*
*初始化 ServerSocket
*/
bool IOCPServer::_InitServerSocket()
{
	SOCKADDR_IN serveraddr;
	int ret = 0;

	// 初始化 Socket 服务
	WSADATA wsaData;
	int nResult;
	nResult = WSAStartup(MAKEWORD(2, 2), &wsaData);
	if (NO_ERROR != nResult)
	{
		std::cout << "初始化 Socket 服务失败!\n";
		return false;
	}

	// 生成服务器完成端口上下文
	m_pServerContext = new PER_SOCKET_CONTEXT(1);

	// 生成 server socket （必须得使用WSASocket来建立Socket，才可以支持重叠IO操作）
	// WSA_FLAG_OVERLAPPED 支持重叠 IO 选项
	m_pServerContext->m_Socket = WSASocket(AF_INET, SOCK_STREAM, 0, NULL, 0, WSA_FLAG_OVERLAPPED);
	if (m_pServerContext->m_Socket == INVALID_SOCKET)
	{
		std::cout << "初始化 server socket 失败,错误代码：" << WSAGetLastError() << std::endl;
		return false;
	}
	std::cout << "初始化 server socket 成功！\n";

	// 将 server socket 绑定到完成端口中(依旧使用  函数)
	// 注意第三个参数，这里直接将 server context 传入了 完成端口，一会儿 GetQueuedCompletionStatus 会把这个参数从那边拿出来，
	// 其实就是一个传参的操作，将这里的 m_pServerContext 传给了 GetQueuedCompletionStatus 函数
	if (NULL ==
		CreateIoCompletionPort((HANDLE)m_pServerContext->m_Socket, m_hIOCompletionPort, (DWORD)m_pServerContext, 0))
	{
		std::cout << "绑定 server socket 到完成端口失败, 错误代码：" << WSAGetLastError() << std::endl;
		// 清空 server socket
		RELEASE_SOCKET(m_pServerContext->m_Socket);
		return false;
	}
	std::cout << "绑定 server socket 到完成端口成功!\n";

	ZeroMemory(&serveraddr, sizeof(serveraddr));
	serveraddr.sin_family = AF_INET;
	serveraddr.sin_addr.s_addr = inet_addr("127.0.0.1");
	serveraddr.sin_port = htons(m_nPort);

	//给 server 绑定 地址信息
	if (SOCKET_ERROR == bind(m_pServerContext->m_Socket, (struct sockaddr*)&serveraddr, sizeof(serveraddr)))
	{
		std::cout << "绑定端口失败！\n";
		return false;
	}

	// 开始监听
	if (SOCKET_ERROR == listen(m_pServerContext->m_Socket, SOMAXCONN))
	{
		std::cout << "监听端口失败！\n";
		return false;
	}

	std::cout << "服务器启动成功，正在监听端口：" << m_nPort << std::endl;

	if (!_GetPAcceptEx())
	{
		_DeInit();
	}

	if (!_GetPGetAcceptExSockAddrs())
	{
		_DeInit();
	}

	for (int i = 0; i < MAX_POST_ACCEPT; i++)
	{
		PER_IO_CONTEXT* pServerIOContext = m_pServerContext->GetNewIoContext();

		std::cout << i << std::endl;

		if (!_PostAccept(pServerIOContext))
		{
			m_pServerContext->RemoveContext(pServerIOContext);
			std::cout << "Accept 失败！\n";
			return false;
		}
	}

	std::cout << "投递 " << MAX_POST_ACCEPT << " 个 AcceptEx 请求完毕\n";
	return true;
}


/*
*获取 AcceptEx 函数指针
*/
bool IOCPServer::_GetPAcceptEx()
{
	GUID GuidAcceptEx = WSAID_ACCEPTEX;
	DWORD dwBytes = 0;
	if (SOCKET_ERROR == WSAIoctl(
		m_pServerContext->m_Socket,
		SIO_GET_EXTENSION_FUNCTION_POINTER,
		&GuidAcceptEx,
		sizeof(GuidAcceptEx),
		&m_lpfuAcceptEx,
		sizeof(m_lpfuAcceptEx),
		&dwBytes,
		NULL,
		NULL))
	{
		std::cout << "获取 AcceptEx 函数指针失败!\n";
		return false;
	}
	return true;
}


/*
*获取 GETAcceptExSockAddrs 函数指针
*/
bool IOCPServer::_GetPGetAcceptExSockAddrs()
{
	GUID GuidGetAcceptExSockAddrs = WSAID_GETACCEPTEXSOCKADDRS;
	DWORD dwBytes = 0;
	if (SOCKET_ERROR == WSAIoctl(
		m_pServerContext->m_Socket,
		SIO_GET_EXTENSION_FUNCTION_POINTER,
		&GuidGetAcceptExSockAddrs,
		sizeof(GuidGetAcceptExSockAddrs),
		&m_lpfnGetAcceptExSockAddrs,
		sizeof(m_lpfnGetAcceptExSockAddrs),
		&dwBytes,
		NULL,
		NULL))
	{
		std::cout << "获取 GetAcceptExSockAddrs 函数指针失败!\n";
		return false;
	}
	return true;
}

/*
*投递 AcceptEx 操作
*参数：
*	pServerIOContext 要投递 AcceptEx 操作的 server IO 上下文
*/
bool IOCPServer::_PostAccept(PER_IO_CONTEXT* pServerIOContext)
{
	if (INVALID_SOCKET == m_pServerContext->m_Socket)
	{
		return false;
	}

	DWORD dwBytes = 0;

	// 此次重叠操作的类型，可以使用 GetQueuedCompletionStatus 接收
	pServerIOContext->m_OpType = ACCEPT_POSTED;

	// 存储 重叠操作数据的缓冲区
	WSABUF *p_wbuf = &pServerIOContext->m_wsaBuf;

	// 重叠操作在内核中的标识，相当于完成端口操作的ID
	OVERLAPPED *p_ol = &pServerIOContext->m_Overlapped;

	// 完成端口要求先生成好接收的 socket
	pServerIOContext->m_ClientSocket = WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED);
	if (INVALID_SOCKET == pServerIOContext->m_ClientSocket)
	{
		std::cout << "创建用于 Accept 的 Socket 失败！错误代码：" << WSAGetLastError();
		return false;
	}

	//投递 AcceptEx
	//bool ret = m_lpfuAcceptEx(m_pServerContext->m_Socket, pServerIOContext->m_ClientSocket, p_wbuf->buf,
	//	p_wbuf->len - ((sizeof(SOCKADDR_IN) + 16) * 2),	// 实际接收数据的缓冲区大小（总缓冲区大小 - 本地地址信息大小 - 远程地址信息大小）
	//	sizeof(SOCKADDR_IN) + 16,	// 本地地址信息保留的字节数，该值必须比使用的传输协议的最大地址长度至少多16个字节。
	//	sizeof(SOCKADDR_IN) + 16,	// 远程地址信息保留的字节数，该值必须比使用的传输协议的最大地址长度至少多16个字节。
	//	&dwBytes,	//用于同步操作的，不用管
	//	p_ol);	// 标识 重叠操作的 重叠结构

	bool ret = m_lpfuAcceptEx(m_pServerContext->m_Socket, pServerIOContext->m_ClientSocket, p_wbuf->buf,
		0,	// 实际接收数据的缓冲区大小（总缓冲区大小 - 本地地址信息大小 - 远程地址信息大小）
		sizeof(SOCKADDR_IN) + 16,	// 本地地址信息保留的字节数，该值必须比使用的传输协议的最大地址长度至少多16个字节。
		sizeof(SOCKADDR_IN) + 16,	// 远程地址信息保留的字节数，该值必须比使用的传输协议的最大地址长度至少多16个字节。
		&dwBytes,	//用于同步操作的，不用管
		p_ol);	// 标识 重叠操作的 重叠结构

	if (!ret)
	{
		if (WSA_IO_PENDING != WSAGetLastError()) {
			std::cout << "投递 AcceptEx 请求失败，错误代码：" << WSAGetLastError() << std::endl;
			return false;
		}
	}

	return true;

}


/*
*投递 Recv 完成端口操作
*参数：
*	pIoContext：需要投递的完成端口操作的上下文对象
*/
bool IOCPServer::_PostRecv(PER_IO_CONTEXT * pIoContext, int len)
{

	if (len > MAX_BUFFER_LEN)
	{
		len = MAX_BUFFER_LEN;
	}

	DWORD dwFlags = 0;
	DWORD dwBytes = 0;

	//用于接收 Recv 返回的数据的缓冲区
	WSABUF *p_wbuf = &pIoContext->m_wsaBuf;

	p_wbuf->len = len;

	// 用于投递重叠操作的重叠结果
	OVERLAPPED *p_ol = &pIoContext->m_Overlapped;

	pIoContext->ResetBuffer();
	pIoContext->m_OpType = RECV_POSTED;

	//投递 WSARecv 请求
	int ret = WSARecv(
		pIoContext->m_ClientSocket, // 要 Recv 数据的Socket
		p_wbuf,		// 接收数据的缓冲区
		1,			// lpBuffers数组中WSABUF结构的数量 
		&dwBytes,	// 如果立即返回，这里会返回接收到的字节数
		&dwFlags,	// 用不上，设为0即可
		p_ol,		// 本次重叠操作的重叠结构体
		NULL);		// 完成端口中用不上，设为 NULL 即可

	if ((SOCKET_ERROR == ret) && (WSA_IO_PENDING != WSAGetLastError()))
	{
		std::cout << "投递 Recv 请求失败！" << std::endl;
		return false;
	}

	return true;
}

bool IOCPServer::_PostSend(PER_IO_CONTEXT * pIoContext)
{
	DWORD dwFlag = MSG_PARTIAL;
	DWORD dwBytes;

	// 要发送的数据
	WSABUF *p_wbuf = &pIoContext->m_wsaBuf;

	// 用于投递重叠操作的重叠结构
	OVERLAPPED *p_ol = &pIoContext->m_Overlapped;

	pIoContext->m_OpType = SEND_POSTED;

	//投递 WSARecv 请求
	int ret = WSASend(
		pIoContext->m_ClientSocket, // 要 Send 数据的Socket
		p_wbuf,		// 要发送的数据
		1,			// lpBuffers数组中WSABUF结构的数量 
		&dwBytes,	// 如果立即返回，这里会返回接收到的字节数
		dwFlag,	// 用不上，设为0即可
		p_ol,		// 本次重叠操作的重叠结构体
		NULL);		// 完成端口中用不上，设为 NULL 即可

	if ((SOCKET_ERROR == ret) && (WSA_IO_PENDING != WSAGetLastError()))
	{
		std::cout << "投递 Send 请求失败！" << std::endl;
		return false;
	}

	return true;
}

bool IOCPServer::_HandleGetMessage(PER_IO_CONTEXT* pIoContext, const char * httpHeader)
{
	pIoContext->m_OpType = SEND_POSTED;
	pIoContext->m_wsaBuf.len = MAX_BUFFER_LEN;
	pIoContext->ResetBuffer();
	const char* response = "HTTP/1.0 200 OK\r\nContent-Length: 11\r\n\r\nhello world";
	memcpy_s(pIoContext->m_szBuffer, pIoContext->m_wsaBuf.len, response, strlen(response));
	pIoContext->m_wsaBuf.len = strlen(response);
	return _PostSend(pIoContext);
	return true;
}

bool IOCPServer::_HandlePostMessage(PER_IO_CONTEXT* pIoContext, const char * httpBody)
{
	pIoContext->m_OpType = SEND_POSTED;
	pIoContext->m_wsaBuf.len = MAX_BUFFER_LEN;
	pIoContext->ResetBuffer();
	const char* response = "HTTP/1.0 200 OK\r\nContent-Length: 11\r\n\r\nhello world";
	memcpy_s(pIoContext->m_szBuffer, pIoContext->m_wsaBuf.len, response, strlen(response));
	pIoContext->m_wsaBuf.len = strlen(response);
	return _PostSend(pIoContext);
	return true;
}

/*
*接收并处理 AcceptEx 的结果
*参数：
*	pSocketContext：本次完成端口接收到的 Socket 上下文（是哪一个 socket 返回的 完成端口操作结果）
*	pIoContext：本次接收到的 完成端口操作上下文（操作返回的数据）
*/
bool IOCPServer::_DoAccept(PER_SOCKET_CONTEXT * pSocketContext, PER_IO_CONTEXT * pIoContext)
{
	SOCKADDR_IN* ClientAddr = NULL;
	SOCKADDR_IN* ServerAddr = NULL;
	int clientLen = sizeof(SOCKADDR_IN);
	int serverLen = sizeof(SOCKADDR_IN);

	// 取出客户端 socket信息及客户端发来的第一条消息
	//m_lpfnGetAcceptExSockAddrs(
	//	pIoContext->m_wsaBuf.buf,	// 用户发送来的第一条数据（这个参数是在 AcceptEx 时绑定的）
	//	pIoContext->m_wsaBuf.len - ((sizeof(SOCKADDR_IN) + 16) * 2),	// 缓冲区大小（AcceptEx 时说过）
	//	sizeof(SOCKADDR_IN) + 16,
	//	sizeof(SOCKADDR_IN) + 16,
	//	(LPSOCKADDR*)&ServerAddr, &serverLen,	// 读取服务器地址信息
	//	(LPSOCKADDR*)&ClientAddr, &clientLen	// 读取客户端地址信息
	//);

	m_lpfnGetAcceptExSockAddrs(
		pIoContext->m_wsaBuf.buf,	// 用户发送来的第一条数据（这个参数是在 AcceptEx 时绑定的）
		0,	// 缓冲区大小（AcceptEx 时说过）
		sizeof(SOCKADDR_IN) + 16,
		sizeof(SOCKADDR_IN) + 16,
		(LPSOCKADDR*)&ServerAddr, &serverLen,	// 读取服务器地址信息
		(LPSOCKADDR*)&ClientAddr, &clientLen	// 读取客户端地址信息
	);

	//SOCKADDR_IN addr[2];
	//int addrLen = sizeof(addr);
	//SecureZeroMemory(&addr, addrLen);
	//getpeername(pIoContext->m_ClientSocket, (PSOCKADDR)&addr, &addrLen);

	//ClientAddr = &addr[0];

	// 显示客户端连接消息
	char str[INET_ADDRSTRLEN];
	inet_ntop(AF_INET, &ClientAddr->sin_addr, str, sizeof(str));
	std::cout << "客户端 " << str << " : " << ntohs(ClientAddr->sin_port) << "连入.\n";
	//std::cout << "收到客户端" << str << " : " << ntohs(ClientAddr->sin_port) << "消息：" << pIoContext->m_wsaBuf.buf << std::endl;

	// 生成该 客户端 使用的 socket 上下文
	PER_SOCKET_CONTEXT* pNewSocketContext = new PER_SOCKET_CONTEXT(1);



	pNewSocketContext->m_Socket = pIoContext->m_ClientSocket;
	memcpy_s(&(pNewSocketContext->m_ClientAddr), sizeof(SOCKADDR_IN), ClientAddr, sizeof(SOCKADDR_IN));

	// 将新加入的客户端 SOCKET 与完成端口绑定
	if (NULL == CreateIoCompletionPort(
		(HANDLE)pNewSocketContext->m_Socket, // 新加入的sockt
		m_hIOCompletionPort,				// 在 start 时创建的 完成端口
		(DWORD)pNewSocketContext,			// 上下文
		0))
	{
		std::cout << "绑定客户端 socket 到完成端口失败!,错误代码：" << GetLastError() << std::endl;
		RELEASE(pNewSocketContext);
		return false;
	}


	// 在 新加入的 客户端上投递第一个 Recv 操作
	// 创建 Recv 操作需要的上下文
	PER_IO_CONTEXT* pNewIoContext = pNewSocketContext->GetNewIoContext();
	// 完成端口操作类型为 Recv
	pNewIoContext->m_OpType = RECV_POSTED;
	pNewIoContext->m_ClientSocket = pNewSocketContext->m_Socket;

	// 投递 Recv 操作
	if (!_PostRecv(pNewIoContext, 1))
	{
		pNewSocketContext->RemoveContext(pNewIoContext);
		return false;
	}

	// 将新加入的socket的上下文添加到 客户端上下文列表中
	_AddToContextList(pNewSocketContext);

	//清空 完成端口上下文对象的缓冲区
	pIoContext->ResetBuffer();
	//继续投递 Accept 操作
	return _PostAccept(pIoContext);
}


/*
*接收到客户端消息执行的操作
*参数：
*	pSocketContext：接收到消息的 socket上下文
*	pIoContext：完成端口返回的数据
*/
bool IOCPServer::_DoRecv(PER_SOCKET_CONTEXT * pSocketContext, PER_IO_CONTEXT * pIoContext, int ret_len)
{
	//显示收到的数据
	SOCKADDR_IN* ClientAddr = &pSocketContext->m_ClientAddr;
	char str[INET_ADDRSTRLEN];
	inet_ntop(AF_INET, &ClientAddr->sin_addr, str, sizeof(str));
	//std::cout << "收到客户端" << str << " : " << ntohs(ClientAddr->sin_port) << "消息：" << pIoContext->m_wsaBuf.buf << std::endl;


	if (pSocketContext->isReadHead)
	{
		pSocketContext->httpHeader[pSocketContext->httpHeaderLen] = pIoContext->m_wsaBuf.buf[0];
		pSocketContext->httpHeaderLen++;

		if (strstr(pSocketContext->httpHeader, "\r\n\r\n") != NULL)
		{
			pSocketContext->isReadHead = FALSE;

			char* tempBuff = strstr(pSocketContext->httpHeader, "content-length");
			if (tempBuff != NULL)
			{

				pSocketContext->content_length = atoi(&(tempBuff[16]));
			}

			if (pSocketContext->content_length != 0)
			{
				return _PostRecv(pIoContext, pSocketContext->content_length);
			}

			// content_length 为 0 直接返回
			std::cout << "---------------------------------------" << std::endl;
			std::cout << pSocketContext->httpHeader << std::endl;
			std::cout << "---------------------------------------" << std::endl;

			int method = _GetRequestMethod(pSocketContext->httpHeader);

			if (method == GET_METHOD)
			{
				_HandleGetMessage(pIoContext, pSocketContext->httpHeader);
			}
			else if (method == POST_METHOD)
			{
				_HandlePostMessage(pIoContext, pSocketContext->httpBody);
			}
			else
			{
				// 关闭 socket
			}

			return true;

		}
		else
		{
			// 继续投递 Recv 请求
			return _PostRecv(pIoContext, 1);
		}

	}
	else
	{
		memcpy_s(pSocketContext->httpBody + pSocketContext->httpBodyLen, pSocketContext->content_length - pSocketContext->httpBodyLen, pIoContext->m_wsaBuf.buf, ret_len);
		pSocketContext->httpBodyLen += ret_len;

		if (pSocketContext->httpBodyLen < pSocketContext->content_length)
		{
			return _PostRecv(pIoContext, pSocketContext->content_length - pSocketContext->httpBodyLen);
		}

		std::cout << "---------------------------------------" << std::endl;
		std::cout << pSocketContext->httpHeader << std::endl;
		std::cout << "---------------------------------------" << std::endl;
		std::cout << pSocketContext->httpBody << std::endl;
		std::cout << "---------------------------------------" << std::endl;

		int method = _GetRequestMethod(pSocketContext->httpHeader);

		if (method == GET_METHOD)
		{
			_HandleGetMessage(pIoContext, pSocketContext->httpHeader);
		}
		else if (method == POST_METHOD)
		{
			_HandlePostMessage(pIoContext, pSocketContext->httpBody);
		}
		else
		{
			// 关闭 socket
		}

		return true;

	}

}

bool IOCPServer::_DoSend(PER_SOCKET_CONTEXT * pSocketContext, PER_IO_CONTEXT * pIoContext, int ret_len)
{
	// 数据没有发完，接着发
	if (ret_len < pIoContext->m_wsaBuf.len)
	{
		char* temp = new char[pIoContext->m_wsaBuf.len];
		ZeroMemory(temp, pIoContext->m_wsaBuf.len);
		memcpy_s(temp, pIoContext->m_wsaBuf.len, pIoContext->m_wsaBuf.buf, (pIoContext->m_wsaBuf.len - ret_len));

		pIoContext->m_wsaBuf.len = pIoContext->m_wsaBuf.len - ret_len;
		pIoContext->ResetBuffer();
		memcpy_s(pIoContext->m_wsaBuf.buf, pIoContext->m_wsaBuf.len, temp, pIoContext->m_wsaBuf.len);
		return _PostSend(pIoContext);
	}

	// 如果发完了就投递下一个 recv
	pSocketContext->isReadHead = TRUE;
	pSocketContext->content_length = 0;
	pSocketContext->httpHeaderLen = 0;
	pSocketContext->httpBodyLen = 0;
	pSocketContext->ResetHttpBuffer();

	pIoContext->ResetBuffer();
	pIoContext->m_OpType = RECV_POSTED;
	_PostRecv(pIoContext, 1);
}



/*
*工作者线程，在这里接处理成端口返回的结果
*/
DWORD WINAPI IOCPServer::_WorkerThread(LPVOID lpParam)
{
	THREADPARAMS_WORKER* pParam = (THREADPARAMS_WORKER*)lpParam;
	IOCPServer* pIOCPServer = (IOCPServer*)pParam->pIOCPModel;
	int nThreadNo = (int)pParam->nThreadNo;

	std::cout << "工作者线程启动，ID：" << nThreadNo << std::endl;

	// 用于接收 重叠操作结果，前面我们也说好，重叠结构是重叠操作在内核操作时的唯一标识
	OVERLAPPED *pOverlapped = NULL;

	// 用于接收 建立完成端口时绑定的 PER_SOCKET_CONTEXT 结构体，我们在绑定server socket 的时候说过（CreateIoCompletionPort）
	// 
	PER_SOCKET_CONTEXT *pSocketContext = NULL;

	// 用于接收 操作完成后接收到的字节数
	DWORD dwBytesTransfered = 0;

	// 循环处理请求，直到接收到 Shutdown 信号为止
	while (WAIT_OBJECT_0 != WaitForSingleObject(pIOCPServer->m_hShutdownEvent, 0))
	{

		// 接收 完成端口操作处理结果
		BOOL ret = GetQueuedCompletionStatus(
			pIOCPServer->m_hIOCompletionPort,	// 完成端口句柄
			&dwBytesTransfered,					// 操作完成后接收到的字节数
			(PULONG_PTR)&pSocketContext,		// 建立完成端口时绑定的 PER_SOCKET_CONTEXT 结构体
			&pOverlapped,						// 这个是我们投递 重叠操作（比如投递AcceptEx）时传入的重叠结构
			INFINITE);		//永不超时

		// 如果收到的是退出标志，则直接退出
		if (EXIT_CODE == (DWORD)pSocketContext)
		{
			break;
		}

		// 如果接收失败
		if (!ret)
		{
			// 处理错误信息
			DWORD dwErr = GetLastError();
			if (pIOCPServer->_HandleError(pSocketContext, dwErr))
			{
				break;
			}
			continue;
		}

		// 根据 pOverlapped 得到 pIoContext
		// m_Overlapped 是 PER_IO_CONTEXT 结构体的一个字段，CONTAINING_RECORD 宏可以 使用 m_Overlapped 字段的地址，得出 PER_IO_CONTEXT 结构体 对象的实际地址
		// 总之这个宏可以从 pOverlapped 得到一个 PER_IO_CONTEXT 结构体 对象
		PER_IO_CONTEXT* pIoContext = CONTAINING_RECORD(pOverlapped, PER_IO_CONTEXT, m_Overlapped);

		// 如果客户端退出
		// 在 send 或者 recv 时 接收到的数据为0
		if ((0 == dwBytesTransfered) && (RECV_POSTED == pIoContext->m_OpType || SEND_POSTED == pIoContext->m_OpType))
		{
			SOCKADDR_IN clientAddr = pSocketContext->m_ClientAddr;
			char str[INET_ADDRSTRLEN];
			inet_ntop(AF_INET, &clientAddr.sin_addr, str, sizeof(str));
			std::cout << "客户端 " << str << " : " << ntohs(clientAddr.sin_port) << " 已关闭！\n";
			pIOCPServer->_RemoveContext(pSocketContext);
			continue;
		}
		else
		{
			switch (pIoContext->m_OpType)
			{
			case ACCEPT_POSTED:
			{
				pIOCPServer->_DoAccept(pSocketContext, pIoContext);
			}
			break;

			case RECV_POSTED:
			{
				pIOCPServer->_DoRecv(pSocketContext, pIoContext, dwBytesTransfered);
			}
			break;

			case SEND_POSTED:
			{
				pIOCPServer->_DoSend(pSocketContext, pIoContext, dwBytesTransfered);
			}
			break;

			default:
				std::cout << "_WorkThread 中接收到操作类型异常\n";
				break;
			}
		}

	}

	std::cout << "工作线程 " << nThreadNo << "退出\n";
	RELEASE(lpParam);
	return 0;

}

bool IOCPServer::_HandleError(PER_SOCKET_CONTEXT * pContext, const DWORD & dwErr)
{
	// 如果是超时
	if (WAIT_TIMEOUT == dwErr)
	{

		if (!_IsSocketAlive(pContext->m_Socket))
		{
			SOCKADDR_IN clientAddr = pContext->m_ClientAddr;
			char str[INET_ADDRSTRLEN];
			inet_ntop(AF_INET, &clientAddr.sin_addr, str, sizeof(str));
			std::cout << "客户端 " << str << " : " << ntohs(clientAddr.sin_port) << " 已关闭！\n";
			_RemoveContext(pContext);
			return true;
		}
		else
		{
			SOCKADDR_IN clientAddr = pContext->m_ClientAddr;
			char str[INET_ADDRSTRLEN];
			inet_ntop(AF_INET, &clientAddr.sin_addr, str, sizeof(str));
			std::cout << "客户端 " << str << " : " << ntohs(clientAddr.sin_port) << "连接超时...\n";
			return true;
		}

	}
	// 客户端异常退出
	else if (ERROR_NETNAME_DELETED == dwErr)
	{
		SOCKADDR_IN clientAddr = pContext->m_ClientAddr;
		char str[INET_ADDRSTRLEN];
		inet_ntop(AF_INET, &clientAddr.sin_addr, str, sizeof(str));
		std::cout << "客户端 " << str << " : " << ntohs(clientAddr.sin_port) << " 已关闭！\n";
		_RemoveContext(pContext);
		return true;
	}

	// 其他错误
	else
	{
		std::cout << "完成端口操作出错，线程退出。错误代码：" << dwErr << std::endl;
		return false;
	}

}

/*
*检查 Socket 是否在连接状态
*参数：
*	s：要检查的 SOCKET
*/
bool IOCPServer::_IsSocketAlive(SOCKET s)
{
	int nByteSent = send(s, "", 0, 0);
	if (-1 == nByteSent) return false;
	return true;
}

/*
*释放所有资源
*/
void IOCPServer::_DeInit()
{
	//删除客户端列表互斥量
	DeleteCriticalSection(&m_csContextList);
	// 关闭用于系统退出的事件句柄
	RELEASE_HANDLE(m_hShutdownEvent);
	for (int i = 0; i < m_nThreads; i++)
	{
		RELEASE_HANDLE(m_phWorkerThreads[i]);
	}
	RELEASE(m_phWorkerThreads);
	// 关闭IOCP句柄
	RELEASE_HANDLE(m_hIOCompletionPort);
	// 关闭 server socket
	RELEASE(m_pServerContext);
	std::cout << "释放资源完毕!\n" << std::endl;
}

/*
*将 Socket 上下文 添加到 客户端上下文数组中
*/
void IOCPServer::_AddToContextList(PER_SOCKET_CONTEXT * pSocketContext)
{
	EnterCriticalSection(&m_csContextList);
	m_arrayClientContext.Add(pSocketContext);
	LeaveCriticalSection(&m_csContextList);
}

void IOCPServer::_RemoveContext(PER_SOCKET_CONTEXT * pSocketContext)
{
	EnterCriticalSection(&m_csContextList);
	for (int i = 0; i < m_arrayClientContext.GetCount(); i++)
	{
		if (pSocketContext == m_arrayClientContext.GetAt(i))
		{
			RELEASE(pSocketContext);
			m_arrayClientContext.RemoveAt(i);
			break;
		}
	}
}

void IOCPServer::_ClearContextList()
{
	EnterCriticalSection(&m_csContextList);
	for (int i = 0; i < m_arrayClientContext.GetCount(); i++)
	{
		delete m_arrayClientContext.GetAt(i);
	}
	m_arrayClientContext.RemoveAll();
	LeaveCriticalSection(&m_csContextList);
}

int IOCPServer::_GetRequestMethod(const char * httpHeard)
{
	char method[5] = { 0 };
	memcpy_s(method, 4, httpHeard, 4);

	if (strstr(method, "GET") != NULL)
	{
		return GET_METHOD;
	}

	if (strstr(method, "POST") != NULL)
	{
		return POST_METHOD;
	}

	return OTHER_METHOD;
}
