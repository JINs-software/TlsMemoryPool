#include "TlsMemPool.h"
#include <iostream>

TlsMemPoolManager<int> g_TlsMemPoolMgr(100, 100);

unsigned __stdcall WorkerThreadFunc(void* arg) {
	std::cout << "hello" << std::endl;
	static __declspec(thread) DWORD tlsIdx = g_TlsMemPoolMgr.AllocTlsMemPool(0);

	int* a = g_TlsMemPoolMgr.GetTlsMemPool().AllocMem();
	g_TlsMemPoolMgr.GetTlsMemPool().FreeMem(a);

	std::cout << "bye" << std::endl;
	return 0;
}

int main() {

	HANDLE thHnd = (HANDLE)_beginthreadex(NULL, 0, WorkerThreadFunc, NULL, 0, NULL);

	WaitForSingleObject(thHnd, INFINITE);

	return 0;
}