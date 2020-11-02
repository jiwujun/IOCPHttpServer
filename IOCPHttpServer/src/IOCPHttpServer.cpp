#include "IOCPHttpServer.h"
#include <iostream>
#include<WS2tcpip.h>

// ÿ�������������ɵ��߳�����������Ϊ2 �� 1��
#define WORKER_THREADS_PER_POCESSOR 2

// ͬʱͶ�ݵ� Accept ���������
#define MAX_POST_ACCEPT 10

// ���ݸ� Worker �̵߳��˳��ź�
#define EXIT_CODE NULL


// �ͷ�ָ���
#define RELEASE(x)	{if(x != NULL){delete x; x = NULL;}}
// �ͷ� Socket ��
#define RELEASE_SOCKET(x)	{if(x != INVALID_SOCKET) { closesocket(x); x=INVALID_SOCKET;}}
// �ͷž�� ��
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
	// ��ʼ���̻߳�����
	InitializeCriticalSection(&m_csContextList);

	// ��ʼ��ϵͳ�˳��¼�
	m_hShutdownEvent = CreateEvent(NULL, TRUE, FALSE, NULL);

	// ��ʼ����ɶ˿�
	if (!_InitIOCP())
	{
		std::cout << "��ʼ����ɶ˿�ʧ��\n";
		return false;
	}
	std::cout << "��ʼ����ɶ˿����!\n";

	if (!_InitServerSocket())
	{
		return false;
	}

	std::cout << "��ʼ�ɹ������ڵȴ��ͻ�������...\n";
	return true;
}

void IOCPServer::Stop()
{
	if (m_pServerContext != NULL && m_pServerContext->m_Socket != INVALID_SOCKET)
	{
		// ����ر���Ϣ���¼�֪ͨ
		SetEvent(m_hShutdownEvent);

		for (int i = 0; i < m_nThreads; i++)
		{
			PostQueuedCompletionStatus(m_hIOCompletionPort, 0, (DWORD)EXIT_CODE, NULL);
		}
		// �ȴ����й������̹߳ر�
		WaitForMultipleObjects(m_nThreads, m_phWorkerThreads, TRUE, INFINITE);

		// ����ͻ����������б�
		_ClearContextList();

		//�ͷ�������Դ
		_DeInit();
		// �ر� socket ����
		WSACleanup();
		std::cout << "�������ѹرգ�\n";
	}
}


/*
*��ʼ����ɶ˿ڣ����ҽ��� �������߳�
*/
bool IOCPServer::_InitIOCP()
{
	// �½� ��ɶ˿�
	m_hIOCompletionPort = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
	if (!m_hIOCompletionPort)
	{
		std::cout << "������ɶ˿�ʧ�ܣ�������룺" << WSAGetLastError() << std::endl;
		return false;
	}

	// ���ݱ����Ĵ�����������������Ӧ���߳���
	m_nThreads = WORKER_THREADS_PER_POCESSOR * _GetNumOfProcessors();

	// ��ʼ���������߳�����	(����ǵ� delete)
	m_phWorkerThreads = new HANDLE[m_nThreads];

	// �����������߳�
	DWORD nThreadID;
	for (int i = 0; i < m_nThreads; i++)
	{
		THREADPARAMS_WORKER* pThreadParams = new THREADPARAMS_WORKER;
		pThreadParams->pIOCPModel = this;
		pThreadParams->nThreadNo = i + 1;
		m_phWorkerThreads[i] = CreateThread(0, 0, _WorkerThread, (void *)pThreadParams, 0, &nThreadID);
	}

	std::cout << "������ " << m_nThreads << " �� �������߳�" << std::endl;
	return true;

}


/*
*��ȡ�����Ĵ���������
*����ֵ��
*	�����Ĵ���������
*/
int IOCPServer::_GetNumOfProcessors()
{
	SYSTEM_INFO si;
	GetSystemInfo(&si);
	return si.dwNumberOfProcessors;
}


/*
*��ʼ�� ServerSocket
*/
bool IOCPServer::_InitServerSocket()
{
	SOCKADDR_IN serveraddr;
	int ret = 0;

	// ��ʼ�� Socket ����
	WSADATA wsaData;
	int nResult;
	nResult = WSAStartup(MAKEWORD(2, 2), &wsaData);
	if (NO_ERROR != nResult)
	{
		std::cout << "��ʼ�� Socket ����ʧ��!\n";
		return false;
	}

	// ���ɷ�������ɶ˿�������
	m_pServerContext = new PER_SOCKET_CONTEXT(1);

	// ���� server socket �������ʹ��WSASocket������Socket���ſ���֧���ص�IO������
	// WSA_FLAG_OVERLAPPED ֧���ص� IO ѡ��
	m_pServerContext->m_Socket = WSASocket(AF_INET, SOCK_STREAM, 0, NULL, 0, WSA_FLAG_OVERLAPPED);
	if (m_pServerContext->m_Socket == INVALID_SOCKET)
	{
		std::cout << "��ʼ�� server socket ʧ��,������룺" << WSAGetLastError() << std::endl;
		return false;
	}
	std::cout << "��ʼ�� server socket �ɹ���\n";

	// �� server socket �󶨵���ɶ˿���(����ʹ��  ����)
	// ע�����������������ֱ�ӽ� server context ������ ��ɶ˿ڣ�һ��� GetQueuedCompletionStatus �������������Ǳ��ó�����
	// ��ʵ����һ�����εĲ������������ m_pServerContext ������ GetQueuedCompletionStatus ����
	if (NULL ==
		CreateIoCompletionPort((HANDLE)m_pServerContext->m_Socket, m_hIOCompletionPort, (DWORD)m_pServerContext, 0))
	{
		std::cout << "�� server socket ����ɶ˿�ʧ��, ������룺" << WSAGetLastError() << std::endl;
		// ��� server socket
		RELEASE_SOCKET(m_pServerContext->m_Socket);
		return false;
	}
	std::cout << "�� server socket ����ɶ˿ڳɹ�!\n";

	ZeroMemory(&serveraddr, sizeof(serveraddr));
	serveraddr.sin_family = AF_INET;
	serveraddr.sin_addr.s_addr = inet_addr("127.0.0.1");
	serveraddr.sin_port = htons(m_nPort);

	//�� server �� ��ַ��Ϣ
	if (SOCKET_ERROR == bind(m_pServerContext->m_Socket, (struct sockaddr*)&serveraddr, sizeof(serveraddr)))
	{
		std::cout << "�󶨶˿�ʧ�ܣ�\n";
		return false;
	}

	// ��ʼ����
	if (SOCKET_ERROR == listen(m_pServerContext->m_Socket, SOMAXCONN))
	{
		std::cout << "�����˿�ʧ�ܣ�\n";
		return false;
	}

	std::cout << "�����������ɹ������ڼ����˿ڣ�" << m_nPort << std::endl;

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
			std::cout << "Accept ʧ�ܣ�\n";
			return false;
		}
	}

	std::cout << "Ͷ�� " << MAX_POST_ACCEPT << " �� AcceptEx �������\n";
	return true;
}


/*
*��ȡ AcceptEx ����ָ��
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
		std::cout << "��ȡ AcceptEx ����ָ��ʧ��!\n";
		return false;
	}
	return true;
}


/*
*��ȡ GETAcceptExSockAddrs ����ָ��
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
		std::cout << "��ȡ GetAcceptExSockAddrs ����ָ��ʧ��!\n";
		return false;
	}
	return true;
}

/*
*Ͷ�� AcceptEx ����
*������
*	pServerIOContext ҪͶ�� AcceptEx ������ server IO ������
*/
bool IOCPServer::_PostAccept(PER_IO_CONTEXT* pServerIOContext)
{
	if (INVALID_SOCKET == m_pServerContext->m_Socket)
	{
		return false;
	}

	DWORD dwBytes = 0;

	// �˴��ص����������ͣ�����ʹ�� GetQueuedCompletionStatus ����
	pServerIOContext->m_OpType = ACCEPT_POSTED;

	// �洢 �ص��������ݵĻ�����
	WSABUF *p_wbuf = &pServerIOContext->m_wsaBuf;

	// �ص��������ں��еı�ʶ���൱����ɶ˿ڲ�����ID
	OVERLAPPED *p_ol = &pServerIOContext->m_Overlapped;

	// ��ɶ˿�Ҫ�������ɺý��յ� socket
	pServerIOContext->m_ClientSocket = WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED);
	if (INVALID_SOCKET == pServerIOContext->m_ClientSocket)
	{
		std::cout << "�������� Accept �� Socket ʧ�ܣ�������룺" << WSAGetLastError();
		return false;
	}

	//Ͷ�� AcceptEx
	//bool ret = m_lpfuAcceptEx(m_pServerContext->m_Socket, pServerIOContext->m_ClientSocket, p_wbuf->buf,
	//	p_wbuf->len - ((sizeof(SOCKADDR_IN) + 16) * 2),	// ʵ�ʽ������ݵĻ�������С���ܻ�������С - ���ص�ַ��Ϣ��С - Զ�̵�ַ��Ϣ��С��
	//	sizeof(SOCKADDR_IN) + 16,	// ���ص�ַ��Ϣ�������ֽ�������ֵ�����ʹ�õĴ���Э�������ַ�������ٶ�16���ֽڡ�
	//	sizeof(SOCKADDR_IN) + 16,	// Զ�̵�ַ��Ϣ�������ֽ�������ֵ�����ʹ�õĴ���Э�������ַ�������ٶ�16���ֽڡ�
	//	&dwBytes,	//����ͬ�������ģ����ù�
	//	p_ol);	// ��ʶ �ص������� �ص��ṹ

	bool ret = m_lpfuAcceptEx(m_pServerContext->m_Socket, pServerIOContext->m_ClientSocket, p_wbuf->buf,
		0,	// ʵ�ʽ������ݵĻ�������С���ܻ�������С - ���ص�ַ��Ϣ��С - Զ�̵�ַ��Ϣ��С��
		sizeof(SOCKADDR_IN) + 16,	// ���ص�ַ��Ϣ�������ֽ�������ֵ�����ʹ�õĴ���Э�������ַ�������ٶ�16���ֽڡ�
		sizeof(SOCKADDR_IN) + 16,	// Զ�̵�ַ��Ϣ�������ֽ�������ֵ�����ʹ�õĴ���Э�������ַ�������ٶ�16���ֽڡ�
		&dwBytes,	//����ͬ�������ģ����ù�
		p_ol);	// ��ʶ �ص������� �ص��ṹ

	if (!ret)
	{
		if (WSA_IO_PENDING != WSAGetLastError()) {
			std::cout << "Ͷ�� AcceptEx ����ʧ�ܣ�������룺" << WSAGetLastError() << std::endl;
			return false;
		}
	}

	return true;

}


/*
*Ͷ�� Recv ��ɶ˿ڲ���
*������
*	pIoContext����ҪͶ�ݵ���ɶ˿ڲ����������Ķ���
*/
bool IOCPServer::_PostRecv(PER_IO_CONTEXT * pIoContext, int len)
{

	if (len > MAX_BUFFER_LEN)
	{
		len = MAX_BUFFER_LEN;
	}

	DWORD dwFlags = 0;
	DWORD dwBytes = 0;

	//���ڽ��� Recv ���ص����ݵĻ�����
	WSABUF *p_wbuf = &pIoContext->m_wsaBuf;

	p_wbuf->len = len;

	// ����Ͷ���ص��������ص����
	OVERLAPPED *p_ol = &pIoContext->m_Overlapped;

	pIoContext->ResetBuffer();
	pIoContext->m_OpType = RECV_POSTED;

	//Ͷ�� WSARecv ����
	int ret = WSARecv(
		pIoContext->m_ClientSocket, // Ҫ Recv ���ݵ�Socket
		p_wbuf,		// �������ݵĻ�����
		1,			// lpBuffers������WSABUF�ṹ������ 
		&dwBytes,	// ����������أ�����᷵�ؽ��յ����ֽ���
		&dwFlags,	// �ò��ϣ���Ϊ0����
		p_ol,		// �����ص��������ص��ṹ��
		NULL);		// ��ɶ˿����ò��ϣ���Ϊ NULL ����

	if ((SOCKET_ERROR == ret) && (WSA_IO_PENDING != WSAGetLastError()))
	{
		std::cout << "Ͷ�� Recv ����ʧ�ܣ�" << std::endl;
		return false;
	}

	return true;
}

bool IOCPServer::_PostSend(PER_IO_CONTEXT * pIoContext)
{
	DWORD dwFlag = MSG_PARTIAL;
	DWORD dwBytes;

	// Ҫ���͵�����
	WSABUF *p_wbuf = &pIoContext->m_wsaBuf;

	// ����Ͷ���ص��������ص��ṹ
	OVERLAPPED *p_ol = &pIoContext->m_Overlapped;

	pIoContext->m_OpType = SEND_POSTED;

	//Ͷ�� WSARecv ����
	int ret = WSASend(
		pIoContext->m_ClientSocket, // Ҫ Send ���ݵ�Socket
		p_wbuf,		// Ҫ���͵�����
		1,			// lpBuffers������WSABUF�ṹ������ 
		&dwBytes,	// ����������أ�����᷵�ؽ��յ����ֽ���
		dwFlag,	// �ò��ϣ���Ϊ0����
		p_ol,		// �����ص��������ص��ṹ��
		NULL);		// ��ɶ˿����ò��ϣ���Ϊ NULL ����

	if ((SOCKET_ERROR == ret) && (WSA_IO_PENDING != WSAGetLastError()))
	{
		std::cout << "Ͷ�� Send ����ʧ�ܣ�" << std::endl;
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
*���ղ����� AcceptEx �Ľ��
*������
*	pSocketContext��������ɶ˿ڽ��յ��� Socket �����ģ�����һ�� socket ���ص� ��ɶ˿ڲ��������
*	pIoContext�����ν��յ��� ��ɶ˿ڲ��������ģ��������ص����ݣ�
*/
bool IOCPServer::_DoAccept(PER_SOCKET_CONTEXT * pSocketContext, PER_IO_CONTEXT * pIoContext)
{
	SOCKADDR_IN* ClientAddr = NULL;
	SOCKADDR_IN* ServerAddr = NULL;
	int clientLen = sizeof(SOCKADDR_IN);
	int serverLen = sizeof(SOCKADDR_IN);

	// ȡ���ͻ��� socket��Ϣ���ͻ��˷����ĵ�һ����Ϣ
	//m_lpfnGetAcceptExSockAddrs(
	//	pIoContext->m_wsaBuf.buf,	// �û��������ĵ�һ�����ݣ������������ AcceptEx ʱ�󶨵ģ�
	//	pIoContext->m_wsaBuf.len - ((sizeof(SOCKADDR_IN) + 16) * 2),	// ��������С��AcceptEx ʱ˵����
	//	sizeof(SOCKADDR_IN) + 16,
	//	sizeof(SOCKADDR_IN) + 16,
	//	(LPSOCKADDR*)&ServerAddr, &serverLen,	// ��ȡ��������ַ��Ϣ
	//	(LPSOCKADDR*)&ClientAddr, &clientLen	// ��ȡ�ͻ��˵�ַ��Ϣ
	//);

	m_lpfnGetAcceptExSockAddrs(
		pIoContext->m_wsaBuf.buf,	// �û��������ĵ�һ�����ݣ������������ AcceptEx ʱ�󶨵ģ�
		0,	// ��������С��AcceptEx ʱ˵����
		sizeof(SOCKADDR_IN) + 16,
		sizeof(SOCKADDR_IN) + 16,
		(LPSOCKADDR*)&ServerAddr, &serverLen,	// ��ȡ��������ַ��Ϣ
		(LPSOCKADDR*)&ClientAddr, &clientLen	// ��ȡ�ͻ��˵�ַ��Ϣ
	);

	//SOCKADDR_IN addr[2];
	//int addrLen = sizeof(addr);
	//SecureZeroMemory(&addr, addrLen);
	//getpeername(pIoContext->m_ClientSocket, (PSOCKADDR)&addr, &addrLen);

	//ClientAddr = &addr[0];

	// ��ʾ�ͻ���������Ϣ
	char str[INET_ADDRSTRLEN];
	inet_ntop(AF_INET, &ClientAddr->sin_addr, str, sizeof(str));
	std::cout << "�ͻ��� " << str << " : " << ntohs(ClientAddr->sin_port) << "����.\n";
	//std::cout << "�յ��ͻ���" << str << " : " << ntohs(ClientAddr->sin_port) << "��Ϣ��" << pIoContext->m_wsaBuf.buf << std::endl;

	// ���ɸ� �ͻ��� ʹ�õ� socket ������
	PER_SOCKET_CONTEXT* pNewSocketContext = new PER_SOCKET_CONTEXT(1);



	pNewSocketContext->m_Socket = pIoContext->m_ClientSocket;
	memcpy_s(&(pNewSocketContext->m_ClientAddr), sizeof(SOCKADDR_IN), ClientAddr, sizeof(SOCKADDR_IN));

	// ���¼���Ŀͻ��� SOCKET ����ɶ˿ڰ�
	if (NULL == CreateIoCompletionPort(
		(HANDLE)pNewSocketContext->m_Socket, // �¼����sockt
		m_hIOCompletionPort,				// �� start ʱ������ ��ɶ˿�
		(DWORD)pNewSocketContext,			// ������
		0))
	{
		std::cout << "�󶨿ͻ��� socket ����ɶ˿�ʧ��!,������룺" << GetLastError() << std::endl;
		RELEASE(pNewSocketContext);
		return false;
	}


	// �� �¼���� �ͻ�����Ͷ�ݵ�һ�� Recv ����
	// ���� Recv ������Ҫ��������
	PER_IO_CONTEXT* pNewIoContext = pNewSocketContext->GetNewIoContext();
	// ��ɶ˿ڲ�������Ϊ Recv
	pNewIoContext->m_OpType = RECV_POSTED;
	pNewIoContext->m_ClientSocket = pNewSocketContext->m_Socket;

	// Ͷ�� Recv ����
	if (!_PostRecv(pNewIoContext, 1))
	{
		pNewSocketContext->RemoveContext(pNewIoContext);
		return false;
	}

	// ���¼����socket����������ӵ� �ͻ����������б���
	_AddToContextList(pNewSocketContext);

	//��� ��ɶ˿������Ķ���Ļ�����
	pIoContext->ResetBuffer();
	//����Ͷ�� Accept ����
	return _PostAccept(pIoContext);
}


/*
*���յ��ͻ�����Ϣִ�еĲ���
*������
*	pSocketContext�����յ���Ϣ�� socket������
*	pIoContext����ɶ˿ڷ��ص�����
*/
bool IOCPServer::_DoRecv(PER_SOCKET_CONTEXT * pSocketContext, PER_IO_CONTEXT * pIoContext, int ret_len)
{
	//��ʾ�յ�������
	SOCKADDR_IN* ClientAddr = &pSocketContext->m_ClientAddr;
	char str[INET_ADDRSTRLEN];
	inet_ntop(AF_INET, &ClientAddr->sin_addr, str, sizeof(str));
	//std::cout << "�յ��ͻ���" << str << " : " << ntohs(ClientAddr->sin_port) << "��Ϣ��" << pIoContext->m_wsaBuf.buf << std::endl;


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

			// content_length Ϊ 0 ֱ�ӷ���
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
				// �ر� socket
			}

			return true;

		}
		else
		{
			// ����Ͷ�� Recv ����
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
			// �ر� socket
		}

		return true;

	}

}

bool IOCPServer::_DoSend(PER_SOCKET_CONTEXT * pSocketContext, PER_IO_CONTEXT * pIoContext, int ret_len)
{
	// ����û�з��꣬���ŷ�
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

	// ��������˾�Ͷ����һ�� recv
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
*�������̣߳�������Ӵ���ɶ˿ڷ��صĽ��
*/
DWORD WINAPI IOCPServer::_WorkerThread(LPVOID lpParam)
{
	THREADPARAMS_WORKER* pParam = (THREADPARAMS_WORKER*)lpParam;
	IOCPServer* pIOCPServer = (IOCPServer*)pParam->pIOCPModel;
	int nThreadNo = (int)pParam->nThreadNo;

	std::cout << "�������߳�������ID��" << nThreadNo << std::endl;

	// ���ڽ��� �ص����������ǰ������Ҳ˵�ã��ص��ṹ���ص��������ں˲���ʱ��Ψһ��ʶ
	OVERLAPPED *pOverlapped = NULL;

	// ���ڽ��� ������ɶ˿�ʱ�󶨵� PER_SOCKET_CONTEXT �ṹ�壬�����ڰ�server socket ��ʱ��˵����CreateIoCompletionPort��
	// 
	PER_SOCKET_CONTEXT *pSocketContext = NULL;

	// ���ڽ��� ������ɺ���յ����ֽ���
	DWORD dwBytesTransfered = 0;

	// ѭ����������ֱ�����յ� Shutdown �ź�Ϊֹ
	while (WAIT_OBJECT_0 != WaitForSingleObject(pIOCPServer->m_hShutdownEvent, 0))
	{

		// ���� ��ɶ˿ڲ���������
		BOOL ret = GetQueuedCompletionStatus(
			pIOCPServer->m_hIOCompletionPort,	// ��ɶ˿ھ��
			&dwBytesTransfered,					// ������ɺ���յ����ֽ���
			(PULONG_PTR)&pSocketContext,		// ������ɶ˿�ʱ�󶨵� PER_SOCKET_CONTEXT �ṹ��
			&pOverlapped,						// ���������Ͷ�� �ص�����������Ͷ��AcceptEx��ʱ������ص��ṹ
			INFINITE);		//������ʱ

		// ����յ������˳���־����ֱ���˳�
		if (EXIT_CODE == (DWORD)pSocketContext)
		{
			break;
		}

		// �������ʧ��
		if (!ret)
		{
			// ���������Ϣ
			DWORD dwErr = GetLastError();
			if (pIOCPServer->_HandleError(pSocketContext, dwErr))
			{
				break;
			}
			continue;
		}

		// ���� pOverlapped �õ� pIoContext
		// m_Overlapped �� PER_IO_CONTEXT �ṹ���һ���ֶΣ�CONTAINING_RECORD ����� ʹ�� m_Overlapped �ֶεĵ�ַ���ó� PER_IO_CONTEXT �ṹ�� �����ʵ�ʵ�ַ
		// ��֮�������Դ� pOverlapped �õ�һ�� PER_IO_CONTEXT �ṹ�� ����
		PER_IO_CONTEXT* pIoContext = CONTAINING_RECORD(pOverlapped, PER_IO_CONTEXT, m_Overlapped);

		// ����ͻ����˳�
		// �� send ���� recv ʱ ���յ�������Ϊ0
		if ((0 == dwBytesTransfered) && (RECV_POSTED == pIoContext->m_OpType || SEND_POSTED == pIoContext->m_OpType))
		{
			SOCKADDR_IN clientAddr = pSocketContext->m_ClientAddr;
			char str[INET_ADDRSTRLEN];
			inet_ntop(AF_INET, &clientAddr.sin_addr, str, sizeof(str));
			std::cout << "�ͻ��� " << str << " : " << ntohs(clientAddr.sin_port) << " �ѹرգ�\n";
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
				std::cout << "_WorkThread �н��յ����������쳣\n";
				break;
			}
		}

	}

	std::cout << "�����߳� " << nThreadNo << "�˳�\n";
	RELEASE(lpParam);
	return 0;

}

bool IOCPServer::_HandleError(PER_SOCKET_CONTEXT * pContext, const DWORD & dwErr)
{
	// ����ǳ�ʱ
	if (WAIT_TIMEOUT == dwErr)
	{

		if (!_IsSocketAlive(pContext->m_Socket))
		{
			SOCKADDR_IN clientAddr = pContext->m_ClientAddr;
			char str[INET_ADDRSTRLEN];
			inet_ntop(AF_INET, &clientAddr.sin_addr, str, sizeof(str));
			std::cout << "�ͻ��� " << str << " : " << ntohs(clientAddr.sin_port) << " �ѹرգ�\n";
			_RemoveContext(pContext);
			return true;
		}
		else
		{
			SOCKADDR_IN clientAddr = pContext->m_ClientAddr;
			char str[INET_ADDRSTRLEN];
			inet_ntop(AF_INET, &clientAddr.sin_addr, str, sizeof(str));
			std::cout << "�ͻ��� " << str << " : " << ntohs(clientAddr.sin_port) << "���ӳ�ʱ...\n";
			return true;
		}

	}
	// �ͻ����쳣�˳�
	else if (ERROR_NETNAME_DELETED == dwErr)
	{
		SOCKADDR_IN clientAddr = pContext->m_ClientAddr;
		char str[INET_ADDRSTRLEN];
		inet_ntop(AF_INET, &clientAddr.sin_addr, str, sizeof(str));
		std::cout << "�ͻ��� " << str << " : " << ntohs(clientAddr.sin_port) << " �ѹرգ�\n";
		_RemoveContext(pContext);
		return true;
	}

	// ��������
	else
	{
		std::cout << "��ɶ˿ڲ��������߳��˳���������룺" << dwErr << std::endl;
		return false;
	}

}

/*
*��� Socket �Ƿ�������״̬
*������
*	s��Ҫ���� SOCKET
*/
bool IOCPServer::_IsSocketAlive(SOCKET s)
{
	int nByteSent = send(s, "", 0, 0);
	if (-1 == nByteSent) return false;
	return true;
}

/*
*�ͷ�������Դ
*/
void IOCPServer::_DeInit()
{
	//ɾ���ͻ����б�����
	DeleteCriticalSection(&m_csContextList);
	// �ر�����ϵͳ�˳����¼����
	RELEASE_HANDLE(m_hShutdownEvent);
	for (int i = 0; i < m_nThreads; i++)
	{
		RELEASE_HANDLE(m_phWorkerThreads[i]);
	}
	RELEASE(m_phWorkerThreads);
	// �ر�IOCP���
	RELEASE_HANDLE(m_hIOCompletionPort);
	// �ر� server socket
	RELEASE(m_pServerContext);
	std::cout << "�ͷ���Դ���!\n" << std::endl;
}

/*
*�� Socket ������ ��ӵ� �ͻ���������������
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
