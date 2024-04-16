#include "DynamicTLS_sample.h"
#include <Windows.h>

DWORD g_dwTlsIndex;

void InitGlobalTlsIndex() {
	g_dwTlsIndex = TlsAlloc();
}

void DynamicTlsSampleFunction(stSomeStruct* psomest)
{
	if (psomest != NULL) {
		// 호출자는 이 함수를 초기화하려 함.

		// 데이터를 저장할 공간이 할당된 적이 있는지 확인
		if (TlsGetValue(g_dwTlsIndex) == NULL) {
			// 공간이 할당되어 있지 않다면, 이 함수는 해당 스레드에 의해 최초로 호출된 경우
			PVOID dynamicAllocSpace = HeapAlloc(GetProcessHeap(), 0, sizeof(*psomest));
			TlsSetValue(g_dwTlsIndex, dynamicAllocSpace);
		}

		// 데이터 저장하기 위한 메모리 공간이 존재, 새롭게 전달된 값을 저장
		memcpy(TlsGetValue(g_dwTlsIndex), psomest, sizeof(*psomest));
	}
	else {
		// 호출자가 앞서 함수를 이미 초기화함. 
		// 앞서 저장된 데이터를 활용하여 임의의 작업을 수행하려 함.
		// 데이터가 저장된 공간을 가리키는 주소 값을 얻음.
		psomest = (stSomeStruct*)TlsGetValue(g_dwTlsIndex);
	}
}
