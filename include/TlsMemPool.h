#pragma once
#include <Windows.h>
#include <map>
#include <unordered_map>
#include <mutex>

//#define ASSERT
//#define MEMORY_USAGE_TRACKING
#define	MEMORY_POOL_ALLOC_FREE_TRACKING

template<typename T>
class TlsMemPoolManager;

/**
* @class TlsMemPool
* @brief TLS에 할당될 메모리 풀
*/
template<typename T>
class TlsMemPool {
	friend class TlsMemPoolManager<T>;
	struct stMemPoolNode {
		T unit;
		alignas(16) stMemPoolNode* next;
	};
private:	
	// private 생성자 -> 임의의 생성을 막는다. 
	// placementNew == true, Alloc / Free 시 placement_new, ~() 소멸자 호출
	// placementNew == false, 메모리 풀에서는 생성자까지 호출된 객체로부터 관리가 시작되어야 함 (240417 논의)
	/**
	* @brief TlsMemPool 생성자(private), TlsMemPool에 대한 관리는 생성 및 관리는 전적으로 TlsMemPoolManager를 통해 이루어짐.
	* 
	* @param unitCnt 메모리 풀에 미리 할당될 객체 단위의 갯수
	* @param capacity 메모리 풀에 미리 할당될 객체 단위의 용량(최대 갯수)
	* @param referenceFlag 참조 플래그가 on이면 할당되는 객체는 참조 관리를 받을 수 있음. IncrementRefCnt() 호출 시 참조 카운트를 증가 시킬 수 있으며, FreeMem()을 통한 반환 시 이 참조 카운트를 통해 반환 여부를 결정. 
	* @param placementNew placementNew가 on이면 할당 시 placement_new를 통해 객체를 생성하고, 반환 시 명시적으로 소멸자를 호출함. 반면 off 시 기본 생성자로 호출된 객체가 관리됨.
	* @todo placement new 방식에서 가변 인자 활용 추가
	*/
	template<typename... Args>
	TlsMemPool(size_t unitCnt, size_t capacity, bool referenceFlag = false, bool placementNew = false, Args... args);
	~TlsMemPool();

public:
	/**
	* @brief 메모리 풀 내 객체 할당
	* @param refCnt 참조를 지원하는 메모리 풀일 경우 참조 카운트를 선 지정 가능
	* @return 메모리 풀 내 객체 주소 반환
	* @todo placement new 방식에서 가변 인자 활용 추가
	*/
	template<typename... Args>
	T* AllocMem(SHORT refCnt = 1, Args... args);

	/**
	* @brief 할당된 객체 반환
	* @param address 할당된 객체 포인터
	*/
	void FreeMem(T* address);

	/**
	* @brief 참조를 지원하는 메모리 풀일 경우 할당받은 메모리 풀 내 객체의 참조를 증가, 참조되지 않는 상황까지 메모리 풀로의 반환을 연기함.
	* @param address 할당된 객체 포인터
	* @refCnt 증가시킬 참조 카운트
	*/
	void IncrementRefCnt(T* address, USHORT refCnt = 1);

	inline size_t GetMemPoolCapacity() { return sizeof(stMemPoolNode) * m_UnitCapacity; }
	inline size_t GetMemPoolSize() { return sizeof(stMemPoolNode) * m_UnitCount; }

private:
	template<typename... Args>
	void InjectNewMem(T* address, Args... args);

private:
	TlsMemPoolManager<T>* m_MemPoolMgr;
	stMemPoolNode* m_FreeFront;
	size_t	m_UnitCount;
	size_t	m_UnitCapacity;
	bool	m_PlacementNewFlag;
	bool	m_ReferenceFlag;
	DWORD	m_ThreadID;
};

/**
* @details
* 
*/
template<typename T>
template<typename... Args>
TlsMemPool<T>::TlsMemPool(size_t unitCnt, size_t capacity, bool referenceFlag,  bool placementNew, Args... args)
	: m_MemPoolMgr(NULL), m_FreeFront(NULL), m_UnitCount(unitCnt), m_UnitCapacity(capacity), m_ReferenceFlag(referenceFlag), m_PlacementNewFlag(placementNew)
{
	m_ThreadID = GetThreadId(GetCurrentThread());
	if (m_UnitCount > 0) {
		m_FreeFront = (stMemPoolNode*)calloc(m_UnitCount, sizeof(stMemPoolNode));
#if defined(ASSERT)
		if (m_FreeFront == NULL) {
			DebugBreak();
		}
#endif

		stMemPoolNode* nodePtr = (stMemPoolNode*)(m_FreeFront);
		for (size_t idx = 0; idx < m_UnitCount; idx++) {
			if (!m_PlacementNewFlag) {
				T* tptr = reinterpret_cast<T*>(nodePtr);
				new (tptr) T(args...);
			}
			nodePtr->next = nodePtr + 1;
			nodePtr += 1;
		}
		nodePtr -= 1;
		nodePtr->next = NULL;	// last tail is null
	}
}

template<typename T>
TlsMemPool<T>::~TlsMemPool() {
	if (m_PlacementNewFlag == false) {
		// 초기 생성자 호출 방식에서는 메모리 풀 자체의 소멸자가 호출될 시 관리 객체들의 소멸자를 호출
		while (m_FreeFront != NULL) {
			reinterpret_cast<T*>(m_FreeFront)->~T();
			m_FreeFront = m_FreeFront->next;
		}
	}
}

template<typename T>
template<typename... Args>
T* TlsMemPool<T>::AllocMem(SHORT refCnt, Args... args) {
	stMemPoolNode* node = NULL;

	if (m_FreeFront == NULL) {			// 할당 가능 공간 부족
		if (m_MemPoolMgr != NULL) {		// 메모리 풀 관리자에게 할당을 요청
			m_MemPoolMgr->Alloc(args...);
		}
		else {
			T* newAlloc = reinterpret_cast<T*>(malloc(sizeof(stMemPoolNode)));
			InjectNewMem(newAlloc, args...);
		}
	}

	node = m_FreeFront;
	if (node != NULL) {
		m_FreeFront = m_FreeFront->next;
		if (m_UnitCount == 0) {
#if defined(ASSERT)
			DebugBreak();
#else
			m_UnitCount = 1;	// temp
#endif
		}
		m_UnitCount--;
	}

	if (m_ReferenceFlag) {
		SHORT* refCntPtr = reinterpret_cast<SHORT*>(reinterpret_cast<PBYTE>(&node->next) + sizeof(stMemPoolNode*));
		refCntPtr -= 1;
		*refCntPtr = refCnt;
	}

	T* ret = reinterpret_cast<T*>(node);
	if (m_PlacementNewFlag) {
		new (ret) T(args...);
	}

#if defined	MEMORY_POOL_ALLOC_FREE_TRACKING
	m_MemPoolMgr->ResetMemPoolUsageCount(true);
#endif
	return ret;
}

template<typename T>
void TlsMemPool<T>::FreeMem(T * address) {
	stMemPoolNode* node = reinterpret_cast<stMemPoolNode*>(address);

	if (m_ReferenceFlag) {
		SHORT* refCntPtr = reinterpret_cast<SHORT*>(reinterpret_cast<PBYTE>(&node->next) + sizeof(stMemPoolNode*));
		refCntPtr -= 1;
		SHORT refCnt = InterlockedDecrement16(refCntPtr);

#if defined(MEMORY_USAGE_TRACKING)
		InterlockedIncrement64((int64*)&m_MemPoolMgr->totalDecrementRefCnt);
#endif
		if (refCnt > 0) {
			return;
		}

		if (refCnt < 0) {
#if defined(ASSERT)
			// 의도되지 않은 흐름
			DebugBreak();
#else
			return;		// temp
#endif
		}
	}

	if (m_PlacementNewFlag) { address->~T(); }

	if (m_UnitCount < m_UnitCapacity) {
		node->next = m_FreeFront;
		m_FreeFront = node;
		m_UnitCount++;
	}
	else { m_MemPoolMgr->Free(address); }

#if defined	MEMORY_POOL_ALLOC_FREE_TRACKING
	m_MemPoolMgr->ResetMemPoolUsageCount(false);
#endif
}

template<typename T>
template<typename... Args>
inline void TlsMemPool<T>::InjectNewMem(T* address, Args... args)
{
	if (!m_PlacementNewFlag) { new (address) T(args...); }

	if (m_UnitCount < m_UnitCapacity) {
		stMemPoolNode* node = reinterpret_cast<stMemPoolNode*>(address);
		node->next = m_FreeFront;
		m_FreeFront = node;
		m_UnitCount++;
	}
	else { m_MemPoolMgr->Free(address); }
}

template<typename T>
inline void TlsMemPool<T>::IncrementRefCnt(T * address, USHORT refCnt) {

	if (m_ReferenceFlag) {
		stMemPoolNode* node = reinterpret_cast<stMemPoolNode*>(address);
		for (USHORT i = 0; i < refCnt; i++) {
			SHORT* refCntPtr = reinterpret_cast<SHORT*>(reinterpret_cast<PBYTE>(&node->next) + sizeof(stMemPoolNode*));
			refCntPtr -= 1;
			SHORT refCntResult = InterlockedIncrement16(refCntPtr);

#if defined(MEMORY_USAGE_TRACKING)
			InterlockedIncrement64((int64*)&m_MemPoolMgr->totalIncrementRefCnt);
#endif
		}
	}
#if defined(ASSERT)
	else DebugBreak();
#endif
}

/**
* @class TlsMemPoolManager
* @brief TlsMemPool을 관리하는 매니저 클래스
*/
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
		T* AllocLFM(size_t& allocCnt);
		void FreeLFM(T* address);
		size_t GetFreeCnt();
		void Resize(size_t resizeCnt);

	private:
		alignas(128) LockFreeNode m_FreeFront;
		short m_Increment = 0;
		const unsigned long long mask = 0x0000'FFFF'FFFF'FFFF;
	};


public:
	TlsMemPoolManager() : TlsMemPoolManager(0, 0) {}
	TlsMemPoolManager(size_t defaultMemUnitCnt = 0, size_t defaultMemUnitCapacity = 0, bool memUnitReferenceFlag = false, bool memUnitPlacementNewFlag = false)
		: m_DefaultMemUnitCnt(defaultMemUnitCnt), m_DefaultMemUnitCapacity(defaultMemUnitCapacity), m_MemUnitReferenceFlag(memUnitReferenceFlag), m_MemUnitPlacementNewFlag(memUnitPlacementNewFlag)
	{
		m_TlsIMainIndex = TlsAlloc();
		m_TlsSurpIndex = TlsAlloc();

		InitializeSRWLock(&m_ThMemPoolMapSrwLock);
	}

	template<typename... Args>
	DWORD AllocTlsMemPool(size_t memUnitCnt = 0, size_t memUnitCapacity = 0, Args... args);

	inline DWORD GetTlsMemPoolIdx() { return m_TlsIMainIndex; }

	inline TlsMemPool<T>& GetTlsMemPool() { return *reinterpret_cast<TlsMemPool<T>*>(TlsGetValue(m_TlsIMainIndex)); }

	template<typename... Args>
	void Alloc(Args... args);
	void Free(T* address);

private:
	DWORD	m_TlsIMainIndex;
	DWORD	m_TlsSurpIndex;
	size_t	m_DefaultMemUnitCnt;
	size_t	m_DefaultMemUnitCapacity;
	bool	m_MemUnitReferenceFlag;
	bool	m_MemUnitPlacementNewFlag;

	std::map<DWORD, LockFreeMemPool*>	m_ThMemPoolMap;
	SRWLOCK								m_ThMemPoolMapSrwLock;

#if defined	MEMORY_POOL_ALLOC_FREE_TRACKING
public:
	UINT64 m_TotalAllocMemCount = 0;
	UINT64 m_TotalFreeMemCount = 0;
	INT64 m_AllocatedMemUnitCount = 0;
	UINT64 m_MallocCount = 0;

	inline void ResetMemPoolUsageCount(bool allocFlag) {
		if (allocFlag) {
			InterlockedIncrement64(&m_AllocatedMemUnitCount);
			InterlockedIncrement64((INT64*)&m_TotalAllocMemCount);
		}
		else {
			InterlockedDecrement64(&m_AllocatedMemUnitCount);
			InterlockedIncrement64((INT64*)&m_TotalFreeMemCount);
		}
	}

	inline UINT64 GetTotalAllocMemCnt() { return m_TotalAllocMemCount; }
	inline UINT64 GetTotalFreeMemCnt() { return m_TotalFreeMemCount; }
	inline INT64 GetAllocatedMemUnitCnt() {
#if defined(ASSERT)
		if (m_AllocatedMemUnitCount < 0) {
			DebugBreak();
		}
#endif
		return m_AllocatedMemUnitCount;
	}
	inline UINT64 GetMallocCount() { return m_MallocCount; }

#elif defined(MEMORY_USAGE_TRACKING)
public:
	// 전체 메모리 정보
	size_t totalAllocMemCnt = 0;
	size_t totalFreeMemCnt = 0;
	size_t totalIncrementRefCnt = 0;
	size_t totalDecrementRefCnt = 0;
	inline size_t GetTotalAllocMemCnt() { return totalAllocMemCnt; }
	inline size_t GetTotalFreeMemCnt() { return totalFreeMemCnt; }
	inline size_t GetAllocatedMemUnitCnt() { return allocatedMemUnitCnt; }
	inline size_t GetTotalIncrementRefCnt() { return totalIncrementRefCnt; }
	inline size_t GetTotalDecrementRefCnt() { return totalDecrementRefCnt; }

	// 스레드 별 메모리 정보
	std::unordered_map<DWORD, stMemoryPoolUseInfo> thMemInfo;
	void ResetMemInfo(size_t tlsMemPoolUnit, size_t mallocCnt) {
		DWORD thID = GetThreadId(GetCurrentThread());
		LockFreeMemPool* lfMemPool = reinterpret_cast<LockFreeMemPool*>(TlsGetValue(m_TlsSurpIndex));
		thMemInfo[thID].tlsMemPoolUnitCnt = tlsMemPoolUnit;
		thMemInfo[thID].lfMemPoolFreeCnt = lfMemPool->GetFreeCnt();
		//thMemInfo[thID].mallocCnt = *(size_t*)TlsGetValue(m_TlsMallocCnt);
		thMemInfo[thID].mallocCnt = mallocCnt;
	}
	
	std::unordered_map<DWORD, stMemoryPoolUseInfo> GetMemInfo() { return thMemInfo; }

#if defined(ALLOC_MEM_LOG)
	// 메모리 Alloc 로그
	std::vector<stAllocMemLog> m_AllocLog;
	USHORT m_AllocLogIndex;

	std::map<UINT_PTR, short> m_AllocMap;
	std::mutex m_AllocMapMtx;
#endif
#endif
};

template<typename T>
template<typename... Args>
inline DWORD TlsMemPoolManager<T>::AllocTlsMemPool(size_t memUnitCnt, size_t memUnitCapacity, Args... args)
{
	if (TlsGetValue(m_TlsIMainIndex) == NULL) {
		if (memUnitCnt == 0) { memUnitCnt = m_DefaultMemUnitCnt; }
		if (memUnitCapacity == 0) { memUnitCapacity = m_DefaultMemUnitCapacity; }

		// TlsMemPool 생성
		TlsMemPool<T>* newTlsMemPool = new TlsMemPool<T>(memUnitCnt, memUnitCapacity, m_MemUnitReferenceFlag, m_MemUnitPlacementNewFlag, args...);
#if defined(ASSERT)
		if (newTlsMemPool == NULL) DebugBreak();
#endif
		newTlsMemPool->m_MemPoolMgr = this;
		TlsSetValue(m_TlsIMainIndex, newTlsMemPool);

		// LockFreeMemPool 생성
		LockFreeMemPool* newLockFreeMemPool = new LockFreeMemPool();
		TlsSetValue(m_TlsSurpIndex, newLockFreeMemPool);

		DWORD thID = GetThreadId(GetCurrentThread());
		
		// 스레드 별 메모리 풀 맵 삽입
		AcquireSRWLockExclusive(&m_ThMemPoolMapSrwLock);
		m_ThMemPoolMap.insert({ thID , newLockFreeMemPool });
		ReleaseSRWLockExclusive(&m_ThMemPoolMapSrwLock);

#if defined(MEMORY_USAGE_TRACKING)
		ResetMemInfo(memUnitCnt, 0);
#endif
	}

	return m_TlsIMainIndex;
}

template<typename T>
template<typename... Args>
void TlsMemPoolManager<T>::Alloc(Args... args)
{
	TlsMemPool<T>* tlsMemPool = reinterpret_cast<TlsMemPool<T>*>(TlsGetValue(m_TlsIMainIndex));
	LockFreeMemPool* lfMemPool = reinterpret_cast<LockFreeMemPool*>(TlsGetValue(m_TlsSurpIndex));

	// 메모리 풀 관리자 차원에서 스레드마다 관리하는 락-프리 메모리 풀에 여분 메모리가 있는지 확인한다.
	// 만약 락-프리 메모리 풀에 여분 메모리가 있다면 두 freeFront를 swap 한다.
	if (lfMemPool->GetFreeCnt() > 0) {
		tlsMemPool->m_FreeFront = reinterpret_cast<TlsMemPool<T>::stMemPoolNode*>(lfMemPool->AllocLFM(tlsMemPool->m_UnitCount));
	}

	// NULL이라면(자신의 락-프리 큐에서 메모리를 얻지 못하였다는 뜻, 
	// 다른 스레드들의 여분 락-프리 메모리 풀에서 얻는다.
	if (tlsMemPool->m_FreeFront != NULL) { return; }
	else {
		do {
			// 가장 크기가 큰 메모리 풀 찾기
			LockFreeMemPool* maxFreeCntPool = NULL;
			DWORD exgTargetThID;
			size_t maxCnt = 0;
			
			AcquireSRWLockShared(&m_ThMemPoolMapSrwLock);
			typename std::map<DWORD, LockFreeMemPool*>::iterator iter = m_ThMemPoolMap.begin();
			for (; iter != m_ThMemPoolMap.end(); iter++) {
				LockFreeMemPool* lfmp = iter->second;
				if (lfmp->GetFreeCnt() > maxCnt) {
					maxFreeCntPool = lfmp;
					exgTargetThID = iter->first;
				}
			}
			ReleaseSRWLockShared(&m_ThMemPoolMapSrwLock);
			

			if (maxFreeCntPool != NULL) {
				tlsMemPool->m_FreeFront = reinterpret_cast<TlsMemPool<T>::stMemPoolNode*>(maxFreeCntPool->AllocLFM(tlsMemPool->m_UnitCount));

#if defined(MEMORY_USAGE_TRACKING)
				//m_ThMemPoolMap
				thMemInfo[exgTargetThID].lfMemPoolFreeCnt = maxFreeCntPool->GetFreeCnt();
#endif
			}
			else {
				T* newAlloc = reinterpret_cast<T*>(malloc(sizeof(TlsMemPool<T>::stMemPoolNode)));
				tlsMemPool->InjectNewMem(newAlloc, args...);
#if defined	MEMORY_POOL_ALLOC_FREE_TRACKING
				InterlockedIncrement64((INT64*)&m_MallocCount);
#endif
			}
		} while (tlsMemPool->m_FreeFront == NULL);
	}
}

template<typename T>
void TlsMemPoolManager<T>::Free(T* address) {
	LockFreeMemPool* lfMemPool = reinterpret_cast<LockFreeMemPool*>(TlsGetValue(m_TlsSurpIndex));
	lfMemPool->FreeLFM(address);
}


template<typename T>
inline T* TlsMemPoolManager<T>::LockFreeMemPool::AllocLFM(size_t& allocCnt)
{
	T* ret = NULL;

	LockFreeNode freeFront;
	do {
		freeFront = m_FreeFront;
	} while (!InterlockedCompareExchange128(reinterpret_cast<LONG64*>(&m_FreeFront), 0, 0, reinterpret_cast<LONG64*>(&freeFront)));

	allocCnt = freeFront.cnt;
	freeFront.cnt = 0;
	ret = reinterpret_cast<T*>(freeFront.ptr & mask);
	return ret;
}

template<typename T>
inline void TlsMemPoolManager<T>::LockFreeMemPool::FreeLFM(T* address)
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
inline size_t TlsMemPoolManager<T>::LockFreeMemPool::GetFreeCnt() { return m_FreeFront.cnt; }
template<typename T>
inline void TlsMemPoolManager<T>::LockFreeMemPool::Resize(size_t resizeCnt)
{
	for (size_t cnt = 0; cnt < resizeCnt; cnt++) {
		T* newNode = malloc(sizeof(T) + sizeof(UINT_PTR));
		Free(newNode);
	}
}
