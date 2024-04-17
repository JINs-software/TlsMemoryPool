#pragma once
#include <Windows.h>
#include <map>
#include <mutex>

#define DEFAULT_MEM_POOL_SIZE	20000
#define DEFAULT_SURPLUS_SIZE	20000

template<typename T>
class TlsMemPoolManager;

template<typename T>
class TlsMemPool {
	template<typename T>
	friend class TlsMemPoolManager;
private:
	TlsMemPool() {}

public:
	T* AllocMem();
	void FreeMem(T* address);

private:
	TlsMemPoolManager<T>* m_MemPoolMgr;
	PBYTE m_FreeFront;
	size_t m_UnitCnt;
	size_t m_MaxFreeListSize;

	PBYTE m_SurplusFront;
	size_t m_SurplusCnt;
};

template<typename T>
class TlsMemPoolManager {
	class LockFreeMemPool {
		struct LockFreeNode {
			volatile UINT_PTR ptr;
			volatile UINT_PTR cnt;

		};
	public:
		LockFreeMemPool() {
			m_FreeFront.ptr = NULL;
			m_FreeFront.cnt = 0;
		}
		T* Alloc(size_t& allocCnt);
		void Free(T* address);
		size_t GetFreeCnt();
		void Resize(size_t resizeCnt);

	private:
		//PBYTE m_FreeFront = NULL;
		//size_t m_UnitCnt = 0;

		alignas(128) LockFreeNode m_FreeFront;

		short m_Increment = 0;
		const unsigned long long mask = 0x0000'FFFF'FFFF'FFFF;
	};


public:
	TlsMemPoolManager();
	TlsMemPoolManager(size_t defaultMemPoolSize, size_t surplusListSize);

	DWORD AllocTlsMemPool(size_t initUnitCnt = 0);
	inline DWORD GetTlsMemPoolIdx() { return m_TlsIMainIndex; }
	inline TlsMemPool<T>& GetTlsMemPool() { return *reinterpret_cast<TlsMemPool<T>*>(TlsGetValue(m_TlsIMainIndex)); }

	void Alloc();
	void Free(T* address);

private:
	DWORD m_TlsIMainIndex;
	DWORD m_TlsSurpIndex;
	size_t m_DefaultMemPoolSize;
	size_t m_SurplusListSize;

	std::map<DWORD, LockFreeMemPool*> m_ThMemPoolMap;
	std::mutex m_ThMemPoolMapMtx;
};

////////////////////////////////////////////////////////////////////////////////
// TlsMemPool
////////////////////////////////////////////////////////////////////////////////
template<typename T>
T* TlsMemPool<T>::AllocMem() {
	PBYTE ret = NULL;

	if (m_FreeFront == NULL) {
		// 할당 공간 부족..
		m_MemPoolMgr->Alloc();
	}

	ret = m_FreeFront;
	if (ret != NULL) {
		m_FreeFront = reinterpret_cast<PBYTE>(*reinterpret_cast<PUINT_PTR>(m_FreeFront + sizeof(T)));
		if (m_UnitCnt == 0) {
			DebugBreak();
		}
		m_UnitCnt--;
	}

	return reinterpret_cast<T*>(ret);
}

template<typename T>
void TlsMemPool<T>::FreeMem(T* address) {
	if (m_UnitCnt < m_MaxFreeListSize) {
		PBYTE ptr = reinterpret_cast<PBYTE>(address);
		ptr += sizeof(T);
		*reinterpret_cast<PUINT_PTR>(ptr) = reinterpret_cast<UINT_PTR>(m_FreeFront);
		m_FreeFront = reinterpret_cast<PBYTE>(address);
		m_UnitCnt++;
	}
	else {
		m_MemPoolMgr->Free(address);
	}
}


////////////////////////////////////////////////////////////////////////////////
// TlsMemPoolManager
////////////////////////////////////////////////////////////////////////////////
template<typename T>
TlsMemPoolManager<T>::TlsMemPoolManager() {
	TlsMemPoolManager(DEFAULT_MEM_POOL_SIZE, DEFAULT_SURPLUS_SIZE);
}
template<typename T>
TlsMemPoolManager<T>::TlsMemPoolManager(size_t defaultMemPoolSize, size_t surplusListSize)
	: m_DefaultMemPoolSize(defaultMemPoolSize), m_SurplusListSize(surplusListSize)
{
	m_TlsIMainIndex = TlsAlloc();
	m_TlsSurpIndex = TlsAlloc();
}

template<typename T>
DWORD TlsMemPoolManager<T>::AllocTlsMemPool(size_t initUnitCnt) {
	if (TlsGetValue(m_TlsIMainIndex) == NULL) {
		// TlsMemPool 생성
		TlsMemPool<T>* newTlsMemPool = new TlsMemPool<T>();
		if (newTlsMemPool == NULL) {
			DebugBreak();
		}
		TlsSetValue(m_TlsIMainIndex, newTlsMemPool);

		// LockFreeMemPool 생성
		LockFreeMemPool* newLockFreeMemPool = new LockFreeMemPool();
		TlsSetValue(m_TlsSurpIndex, newLockFreeMemPool);

		DWORD thID = GetThreadId(GetCurrentThread());
		{
			std::lock_guard<std::mutex> lockGuard(m_ThMemPoolMapMtx);
			m_ThMemPoolMap.insert({ thID , newLockFreeMemPool });
		}

		newTlsMemPool->m_MemPoolMgr = this;
		if (initUnitCnt == 0) {
			newTlsMemPool->m_UnitCnt = m_DefaultMemPoolSize;
		}
		else {
			newTlsMemPool->m_UnitCnt = initUnitCnt;
		}

		newTlsMemPool->m_MaxFreeListSize = m_DefaultMemPoolSize;

		// The calloc function allocates storage space for an array of number elements, each of length size bytes.Each element is initialized to 0.
		newTlsMemPool->m_FreeFront = (PBYTE)calloc(newTlsMemPool->m_UnitCnt, sizeof(T) + sizeof(UINT_PTR));
		PBYTE ptr = newTlsMemPool->m_FreeFront;
		for (size_t idx = 0; idx < newTlsMemPool->m_UnitCnt - 1; idx++) {
			ptr += sizeof(T);
			*(reinterpret_cast<PUINT_PTR>(ptr)) = reinterpret_cast<UINT_PTR>(ptr + sizeof(UINT_PTR));
			ptr += sizeof(UINT_PTR);
		}

		if (newTlsMemPool->m_FreeFront == NULL) {
			DebugBreak();
		}
	}

	return m_TlsIMainIndex;
}

template<typename T>
void TlsMemPoolManager<T>::Alloc()
{
	TlsMemPool<T>* tlsMemPool = reinterpret_cast<TlsMemPool<T>*>(TlsGetValue(m_TlsIMainIndex));
	LockFreeMemPool* lfMemPool = reinterpret_cast<LockFreeMemPool*>(TlsGetValue(m_TlsSurpIndex));
	// 자신의 스레드의 SurplusFront 확인
	// NULL이 아니라면, m_FreeFront와 m_SurplusFront SWAP..
	if (lfMemPool->GetFreeCnt() > 0) {
		tlsMemPool->m_FreeFront = reinterpret_cast<PBYTE>(lfMemPool->Alloc(tlsMemPool->m_UnitCnt));
	}

	// NULL이라면, 다른 스레드들의 SurplusFront와 SWAP
	if (tlsMemPool->m_FreeFront != NULL) {
		return;
	}
	else {
		do {
			// 가장 크기가 큰 메모리 풀 찾기
			LockFreeMemPool* maxFreeCntPool = NULL;
			size_t maxCnt = 0;
			{
				std::lock_guard<std::mutex> lockGuard(m_ThMemPoolMapMtx);

				typename std::map<DWORD, LockFreeMemPool*>::iterator iter = m_ThMemPoolMap.begin();
				for (; iter != m_ThMemPoolMap.end(); iter++) {
					LockFreeMemPool* lfmp = iter->second;
					if (lfmp->GetFreeCnt() > maxCnt) {
						maxFreeCntPool = lfmp;
					}
				}
			}

			if (maxFreeCntPool != NULL) {
				tlsMemPool->m_FreeFront = reinterpret_cast<PBYTE>(maxFreeCntPool->Alloc(tlsMemPool->m_UnitCnt));
			}
			else {
				T* newAlloc = reinterpret_cast<T*>(malloc(sizeof(T) + sizeof(UINT_PTR)));
				tlsMemPool->FreeMem(newAlloc);
			}
		} while (tlsMemPool->m_FreeFront == NULL);
	}

	// 다른 스레드들의 SurplusFront가 모두 NULL이라면, 
	// 할당
}

template<typename T>
void TlsMemPoolManager<T>::Free(T* address) {
	LockFreeMemPool* lfMemPool = reinterpret_cast<LockFreeMemPool*>(TlsGetValue(m_TlsSurpIndex));
	lfMemPool->Free(address);
}


template<typename T>
inline T* TlsMemPoolManager<T>::LockFreeMemPool::Alloc(size_t& allocCnt)
{
	T* ret = NULL;

	LockFreeNode freeFront;
	do {
		freeFront = m_FreeFront;
	} while (!InterlockedCompareExchange128(reinterpret_cast<LONG64*>(&m_FreeFront), 0, 0, reinterpret_cast<LONG64*>(&freeFront)));

	allocCnt = freeFront.cnt;
	ret = reinterpret_cast<T*>(freeFront.ptr & mask);
	return ret;
}

template<typename T>
inline void TlsMemPoolManager<T>::LockFreeMemPool::Free(T* address)
{
	if (address != NULL) {
		UINT_PTR increment = InterlockedIncrement16(&m_Increment);
		increment <<= (64 - 16);

		PBYTE ptr = (PBYTE)address;
		ptr += sizeof(T);

		LockFreeNode freeFront;
		do {
			freeFront = m_FreeFront;
			*reinterpret_cast<PUINT_PTR>(ptr) = static_cast<UINT_PTR>(freeFront.ptr) & mask;
		} while (!InterlockedCompareExchange128(reinterpret_cast<LONG64*>(&m_FreeFront), freeFront.cnt + 1, reinterpret_cast<UINT_PTR>(address) ^ increment, reinterpret_cast<LONG64*>(&freeFront)));
	}
}

template<typename T>
inline size_t TlsMemPoolManager<T>::LockFreeMemPool::GetFreeCnt()
{
	return m_FreeFront.cnt;
}

template<typename T>
inline void TlsMemPoolManager<T>::LockFreeMemPool::Resize(size_t resizeCnt)
{
	for (size_t cnt = 0; cnt < resizeCnt; cnt++) {
		T* newNode = malloc(sizeof(T) + sizeof(UINT_PTR));
		Free(newNode);
	}
}
