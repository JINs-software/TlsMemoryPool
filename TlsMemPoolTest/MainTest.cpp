/*
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
*/

#include "TlsMemPool.h"
#include "JBuffer.h"

template <UINT Capacity>
class SerialBuffer
{
public:
	SerialBuffer() : jbuff(Capacity) {}

	JBuffer jbuff;

	inline UINT Enqueue(const BYTE* data, UINT uiSize) { return jbuff.Enqueue(data, uiSize); }
	// 이 외 JBuffer 퍼블릭 함수 랩핑
	// ..
};

int main() {
	//TlsMemPoolManager<SerialBuffer<100>, true, true> tlsMemPoolMgr(100, 100);
	//
	//tlsMemPoolMgr.AllocTlsMemPool();
	//SerialBuffer<100>* sb = tlsMemPoolMgr.GetTlsMemPool().AllocMem();

	TlsMemPoolManager<JBuffer, true, false> tlsMemPoolManager(100, 100);
	tlsMemPoolManager.AllocTlsMemPool(100, 100, 5000);
	auto ptr = tlsMemPoolManager.GetTlsMemPool().AllocMem();

}

