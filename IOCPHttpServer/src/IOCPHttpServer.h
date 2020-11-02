#pragma once
#define _WINSOCK_DEPRECATED_NO_WARNINGS 1

#include <afxtempl.h>
#include <WinSock2.h>
#include <MSWSock.h>

#pragma comment(lib, "ws2_32.lib")

#define MAX_BUFFER_LEN 4096
#define SERVER_PORT 10086


// �ص�I/O��������
typedef enum _OPERATION_TYPE
{
	ACCEPT_POSTED,	// ��־ Ͷ�ݵ�Accept����
	SEND_POSTED,	// ��־ Ͷ�ݵķ��Ͳ���
	RECV_POSTED,	// ��־ Ͷ�ݵĽ��ղ���
	NULL_POSTED		// ���ڳ�ʼ����������
}OPERATION_TYPE;


typedef enum _HTTP_REQUEST_METHOD
{
	GET_METHOD,		// GET ����
	POST_METHOD,	// POST ����
	OTHER_METHOD	// δ֪����
}HTTP_REQUEST_METHOD;


// �����ص�I/O�����������ݵĽṹ��
typedef struct _PER_IO_CONTEXT
{
	OVERLAPPED m_Overlapped;			// �����ص� I/O ������ �ص��ṹ�壨����Ϊ��һ����Ա��
	SOCKET m_ClientSocket;					// �����ص�I/O������ socket
	WSABUF m_wsaBuf;					// wsa ����Ļ����������ڸ��ص�������������
	char m_szBuffer[MAX_BUFFER_LEN];	// ���屣�� m_wsaBuf ���ݵĵط�������Ļ�������
	OPERATION_TYPE m_OpType;			// �����ص�����������

	// ��ʼ��
	// �������Ϊ�޲ι��캯���Ļ����������
	_PER_IO_CONTEXT(int i)
	{
		ZeroMemory(&m_Overlapped, sizeof(m_Overlapped));
		ZeroMemory(m_szBuffer, MAX_BUFFER_LEN);
		m_ClientSocket = INVALID_SOCKET;
		m_wsaBuf.buf = m_szBuffer;
		m_wsaBuf.len = MAX_BUFFER_LEN;
		m_OpType = NULL_POSTED;
	}

	// �ͷ� socket
	~_PER_IO_CONTEXT()
	{
		if (m_ClientSocket != INVALID_SOCKET)
		{
			closesocket(m_ClientSocket);
			m_ClientSocket = INVALID_SOCKET;
		}
	}

	// ���û�����
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

	// ��ʼ��
	// �������Ϊ�޲ι��캯���Ļ����������
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

	// �ͷ���Դ
	~_PER_SOCKET_CONTEXT()
	{
		if (m_Socket != INVALID_SOCKET)
		{
			closesocket(m_Socket);
			m_Socket = INVALID_SOCKET;
		}

		// �ͷŵ����� I/O����������
		for (int i = 0; i < m_arrayIoContext.GetCount(); i++)
		{
			delete m_arrayIoContext.GetAt(i);
		}
		m_arrayIoContext.RemoveAll();
	}

	// ��ȡһ���µ� IO������
	_PER_IO_CONTEXT* GetNewIoContext()
	{
		_PER_IO_CONTEXT* p = new _PER_IO_CONTEXT(1);
		m_arrayIoContext.Add(p);
		return p;
	}

	// ��������ɾ��һ��ָ���� IO������
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


//���ڽ��� �������̵߳Ĳ����ṹ��
class IOCPServer;
typedef struct _tagThreadParams_WORKER
{
	IOCPServer* pIOCPModel;	// IOCPServer ����ָ�룬���ڵ��÷���
	int nThreadNo;			// �̱߳��
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
	bool _InitIOCP();					// ��ʼ����ɶ˿�
	int _GetNumOfProcessors();			// ��ȡ���������������
	//bool LoadSocketLib();
	bool _InitServerSocket();			// ��ʼ�� server socket

	bool _GetPAcceptEx();				// ��ȡ GetPAcceptEx ����ָ��
	bool _GetPGetAcceptExSockAddrs();	// ��ȡ GetAcceptExSockAddrs ����ָ��

	bool _PostAccept(PER_IO_CONTEXT* pServerIOContext);	// Ͷ�� Accept ����
	bool _PostRecv(PER_IO_CONTEXT* pIoContext, int len);			// Ͷ�� Recv ����
	bool _PostSend(PER_IO_CONTEXT* pIoContext);
	bool _HandleGetMessage(PER_IO_CONTEXT* pIoContext, const char* httpHeader);
	bool _HandlePostMessage(PER_IO_CONTEXT* pIoContext, const char* httpBody);


	bool _DoAccept(PER_SOCKET_CONTEXT* pSocketContext, PER_IO_CONTEXT* pIoContext);	// ���� Accept ���
	bool _DoRecv(PER_SOCKET_CONTEXT* pSocketContext, PER_IO_CONTEXT* pIoContext, int ret_len);	// ���� Recv ���
	bool _DoSend(PER_SOCKET_CONTEXT* pSocketContext, PER_IO_CONTEXT* pIoContext, int ret_len);



	static DWORD WINAPI  _WorkerThread(LPVOID arg);							// �������߳�
	bool _HandleError(PER_SOCKET_CONTEXT* pContext, const DWORD& dwErr);	// ���� ��ȡ��ɶ˿�ʱ�Ĵ�����Ϣ
	bool _IsSocketAlive(SOCKET s);		// ��� Socket �Ƿ�����
	void _DeInit();						// �ͷ�������Դ	

	void _AddToContextList(PER_SOCKET_CONTEXT* pSocketContext);	// ���¼���� SOCKET��������ӵ�������
	void _RemoveContext(PER_SOCKET_CONTEXT *pSocketContext);	// �ӿͻ���������������ɾ���ض��Ŀͻ���
	void _ClearContextList();									// ��տͻ�������������

	int _GetRequestMethod(const char* httpHeard);


private:
	HANDLE m_hIOCompletionPort;				// ��ɶ˿ھ��
	int m_nThreads;							// �������߳�����
	HANDLE* m_phWorkerThreads;				// �������߳̾������
	PER_SOCKET_CONTEXT* m_pServerContext;	// ��������ɶ˿�������
	int m_nPort;							// server �����Ķ˿�
	LPFN_ACCEPTEX m_lpfuAcceptEx;			// AcceptEx ����ָ��
	LPFN_GETACCEPTEXSOCKADDRS m_lpfnGetAcceptExSockAddrs;	// GetAcceptSockAddrs ����ָ��
	HANDLE m_hShutdownEvent;				// ����֪ͨ �������߳� �˳����¼���Ϊ���ܹ����õ��˳�����

	CArray<PER_SOCKET_CONTEXT*> m_arrayClientContext;	// �ͻ���Socket �� Context ��Ϣ
	CRITICAL_SECTION m_csContextList;		// ���� arraryClientContext �����Ļ�����

};


