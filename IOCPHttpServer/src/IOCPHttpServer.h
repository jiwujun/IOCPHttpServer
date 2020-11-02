#pragma once
#define _WINSOCK_DEPRECATED_NO_WARNINGS 1

#include <afxtempl.h>
#include <WinSock2.h>
#include <MSWSock.h>

#pragma comment(lib, "ws2_32.lib")

#define MAX_BUFFER_LEN 4096
#define SERVER_PORT 10086


// 重叠I/O操作类型
typedef enum _OPERATION_TYPE
{
	ACCEPT_POSTED,	// 标志 投递的Accept操作
	SEND_POSTED,	// 标志 投递的发送操作
	RECV_POSTED,	// 标志 投递的接收操作
	NULL_POSTED		// 用于初始化，无意义
}OPERATION_TYPE;


typedef enum _HTTP_REQUEST_METHOD
{
	GET_METHOD,		// GET 请求
	POST_METHOD,	// POST 请求
	OTHER_METHOD	// 未知请求
}HTTP_REQUEST_METHOD;


// 用于重叠I/O操作传递数据的结构体
typedef struct _PER_IO_CONTEXT
{
	OVERLAPPED m_Overlapped;			// 用于重叠 I/O 操作的 重叠结构体（必须为第一个成员）
	SOCKET m_ClientSocket;					// 本次重叠I/O操作的 socket
	WSABUF m_wsaBuf;					// wsa 定义的缓冲区，用于给重叠操作传递数据
	char m_szBuffer[MAX_BUFFER_LEN];	// 具体保存 m_wsaBuf 数据的地方（正真的缓冲区）
	OPERATION_TYPE m_OpType;			// 本次重叠操作的类型

	// 初始化
	// 如果定义为无参构造函数的话，不会调用
	_PER_IO_CONTEXT(int i)
	{
		ZeroMemory(&m_Overlapped, sizeof(m_Overlapped));
		ZeroMemory(m_szBuffer, MAX_BUFFER_LEN);
		m_ClientSocket = INVALID_SOCKET;
		m_wsaBuf.buf = m_szBuffer;
		m_wsaBuf.len = MAX_BUFFER_LEN;
		m_OpType = NULL_POSTED;
	}

	// 释放 socket
	~_PER_IO_CONTEXT()
	{
		if (m_ClientSocket != INVALID_SOCKET)
		{
			closesocket(m_ClientSocket);
			m_ClientSocket = INVALID_SOCKET;
		}
	}

	// 重置缓冲区
	void ResetBuffer()
	{
		ZeroMemory(m_szBuffer, MAX_BUFFER_LEN);
	}

}PER_IO_CONTEXT, *PPER_IO_CONTEXT;


typedef struct _PER_SOCKET_CONTEXT
{
	SOCKET m_Socket;
	SOCKADDR_IN m_ClientAddr;
	CArray<_PER_IO_CONTEXT*> m_arrayIoContext;

	int content_length;
	bool isReadHead;

	int httpHeaderLen;
	int httpBodyLen;
	char httpHeader[4096];
	char httpBody[4096];

	// 初始化
	// 如果定义为无参构造函数的话，不会调用
	_PER_SOCKET_CONTEXT(int i)
	{
		m_Socket = INVALID_SOCKET;
		ZeroMemory(&m_ClientAddr, 0, sizeof(m_ClientAddr));

		content_length = 0;
		isReadHead = TRUE;

		httpHeaderLen = 0;
		httpBodyLen = 0;
		ResetHttpBuffer();
	}

	// 释放资源
	~_PER_SOCKET_CONTEXT()
	{
		if (m_Socket != INVALID_SOCKET)
		{
			closesocket(m_Socket);
			m_Socket = INVALID_SOCKET;
		}

		// 释放掉所有 I/O上下文数据
		for (int i = 0; i < m_arrayIoContext.GetCount(); i++)
		{
			delete m_arrayIoContext.GetAt(i);
		}
		m_arrayIoContext.RemoveAll();
	}

	// 获取一个新的 IO上下文
	_PER_IO_CONTEXT* GetNewIoContext()
	{
		_PER_IO_CONTEXT* p = new _PER_IO_CONTEXT(1);
		m_arrayIoContext.Add(p);
		return p;
	}

	// 从数组中删除一个指定的 IO上下文
	void RemoveContext(_PER_IO_CONTEXT* pContext)
	{
		if (!pContext)
		{
			return;
		}

		for (int i = 0; i < m_arrayIoContext.GetCount(); i++)
		{
			if (pContext == m_arrayIoContext.GetAt(i))
			{
				delete pContext;
				pContext = NULL;
				m_arrayIoContext.RemoveAt(i);
				break;
			}
		}
	}

	void ResetHttpBuffer() {
		ZeroMemory(httpHeader, 4096);
		ZeroMemory(httpBody, 4096);
	}

} PER_SOCKET_CONTEXT, *PPER_SOCKET_CONTEXT;


//用于建立 工作者线程的参数结构体
class IOCPServer;
typedef struct _tagThreadParams_WORKER
{
	IOCPServer* pIOCPModel;	// IOCPServer 对象指针，用于调用方法
	int nThreadNo;			// 线程编号
}THREADPARAMS_WORKER, *PTHREADPARAMS_WORKER;


class IOCPServer
{
public:
	IOCPServer();
	~IOCPServer();

public:
	bool Start();
	void Stop();

protected:
	bool _InitIOCP();					// 初始化完成端口
	int _GetNumOfProcessors();			// 获取计算机处理器数量
	//bool LoadSocketLib();
	bool _InitServerSocket();			// 初始化 server socket

	bool _GetPAcceptEx();				// 获取 GetPAcceptEx 函数指针
	bool _GetPGetAcceptExSockAddrs();	// 获取 GetAcceptExSockAddrs 函数指针

	bool _PostAccept(PER_IO_CONTEXT* pServerIOContext);	// 投递 Accept 请求
	bool _PostRecv(PER_IO_CONTEXT* pIoContext, int len);			// 投递 Recv 请求
	bool _PostSend(PER_IO_CONTEXT* pIoContext);
	bool _HandleGetMessage(PER_IO_CONTEXT* pIoContext, const char* httpHeader);
	bool _HandlePostMessage(PER_IO_CONTEXT* pIoContext, const char* httpBody);


	bool _DoAccept(PER_SOCKET_CONTEXT* pSocketContext, PER_IO_CONTEXT* pIoContext);	// 处理 Accept 结果
	bool _DoRecv(PER_SOCKET_CONTEXT* pSocketContext, PER_IO_CONTEXT* pIoContext, int ret_len);	// 处理 Recv 结果
	bool _DoSend(PER_SOCKET_CONTEXT* pSocketContext, PER_IO_CONTEXT* pIoContext, int ret_len);



	static DWORD WINAPI  _WorkerThread(LPVOID arg);							// 工作者线程
	bool _HandleError(PER_SOCKET_CONTEXT* pContext, const DWORD& dwErr);	// 处理 获取完成端口时的错误信息
	bool _IsSocketAlive(SOCKET s);		// 检查 Socket 是否正常
	void _DeInit();						// 释放所有资源	

	void _AddToContextList(PER_SOCKET_CONTEXT* pSocketContext);	// 将新加入的 SOCKET上下文添加到数组中
	void _RemoveContext(PER_SOCKET_CONTEXT *pSocketContext);	// 从客户端上下文数组中删除特定的客户端
	void _ClearContextList();									// 清空客户端上下文数组

	int _GetRequestMethod(const char* httpHeard);


private:
	HANDLE m_hIOCompletionPort;				// 完成端口句柄
	int m_nThreads;							// 工作者线程数量
	HANDLE* m_phWorkerThreads;				// 工作者线程句柄数组
	PER_SOCKET_CONTEXT* m_pServerContext;	// 服务器完成端口上下文
	int m_nPort;							// server 监听的端口
	LPFN_ACCEPTEX m_lpfuAcceptEx;			// AcceptEx 函数指针
	LPFN_GETACCEPTEXSOCKADDRS m_lpfnGetAcceptExSockAddrs;	// GetAcceptSockAddrs 函数指针
	HANDLE m_hShutdownEvent;				// 用来通知 工作者线程 退出的事件，为了能够更好的退出程序

	CArray<PER_SOCKET_CONTEXT*> m_arrayClientContext;	// 客户端Socket 的 Context 信息
	CRITICAL_SECTION m_csContextList;		// 用于 arraryClientContext 操作的互斥量

};


