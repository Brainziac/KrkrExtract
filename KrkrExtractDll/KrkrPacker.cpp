#include "KrkrPacker.h"
#include "XP3Parser.h"
#include "KrkrExtend.h"
#include "Adler32Stream.h"
#include "mt64.h"

static WCHAR* PackingFormatString = L"Packing[%d/%d]";

static KrkrPacker LocalKrkrPacker;

wstring ToLowerString(LPCWSTR lpString)
{
	wstring Result;

	for (LONG_PTR i = 0; i < StrLengthW(lpString); i++)
	{
		Result += (WCHAR)CHAR_LOWER(lpString[i]);
	}
	return Result;
}


KrkrPacker::KrkrPacker() :
	KrkrPackType(PackInfo::UnknownPack),
	M2Hash(0),
	DecryptionKey(0),
	pfProc(nullptr),
	XP3EncryptionFlag(TRUE),
	hThread((HANDLE)-1),
	ThreadId(0),
	InfoNameZeroEnd(TRUE),
	M2NameZeroEnd(TRUE)
{
	KrkrPackType = PackInfo::UnknownPack;
	M2Hash = 0x23333333;
	DecryptionKey = 0;
	pfProc = nullptr;
	XP3EncryptionFlag = TRUE;
	hThread = INVALID_HANDLE_VALUE;
	ThreadId = 0;
	InfoNameZeroEnd = TRUE;
	M2NameZeroEnd = TRUE;

	RtlZeroMemory(&SenrenBankaInfo, sizeof(SenrenBankaInfo));

	pfProc = GlobalData::GetGlobalData()->pfGlobalXP3Filter;

	if (GlobalData::GetGlobalData()->DebugOn)
		PrintConsoleW(L"Filter Addr : 0x%08x\n", pfProc);
}


Void NTAPI KrkrPacker::Init()
{
	KrkrPackType = PackInfo::UnknownPack;
	DecryptionKey = 0;
	pfProc = GlobalData::GetGlobalData()->pfGlobalXP3Filter;
	XP3EncryptionFlag = TRUE;
	hThread = INVALID_HANDLE_VALUE;
	ThreadId = 0;
	InfoNameZeroEnd = TRUE;
	M2NameZeroEnd = TRUE;
	GlobalData::GetGlobalData()->CurrentTempFileName = L"KrkrzTempWorker.xp3";
	RtlZeroMemory(&SenrenBankaInfo, sizeof(SenrenBankaInfo));

	ThreadId = 0;
}

KrkrPacker::~KrkrPacker()
{
}

VOID WINAPI KrkrPacker::DecryptWorker(ULONG EncryptOffset, PBYTE pBuffer, ULONG BufferSize, ULONG Hash)
{
	tTVPXP3ExtractionFilterInfo Info(0, pBuffer, BufferSize, Hash);
	if (pfProc != nullptr)
	{
		pfProc(&Info);
	}
}

NTSTATUS NTAPI KrkrPacker::GetSenrenBankaPackInfo(PBYTE IndexData, ULONG IndexSize, NtFileDisk& file)
{
	NTSTATUS                Status;
	ULONG                   iPos;
	PBYTE                   CompressedBuffer, IndexBuffer;
	ULONG                   DecompSize;
	GlobalData*             Handle;


	Handle = GlobalData::GetGlobalData();
	iPos   = 0;

	SenrenBankaInfo.Magic = *(PDWORD)(IndexData + iPos);
	iPos += 4;
	SenrenBankaInfo.ChunkSize.QuadPart = *(PULONG64)(IndexData + iPos);
	iPos += 8;
	SenrenBankaInfo.Offset.QuadPart = *(PULONG64)(IndexData + iPos);
	iPos += 8;
	SenrenBankaInfo.OriginalSize = *(PDWORD)(IndexData + iPos);
	iPos += 4;
	SenrenBankaInfo.ArchiveSize = *(PDWORD)(IndexData + iPos);
	iPos += 4;
	SenrenBankaInfo.LengthOfProduct = *(PUSHORT)(IndexData + iPos);
	iPos += 2;
	RtlZeroMemory(SenrenBankaInfo.ProductName, countof(SenrenBankaInfo.ProductName) * sizeof(WCHAR));
	RtlCopyMemory(SenrenBankaInfo.ProductName, (IndexData + iPos), SenrenBankaInfo.LengthOfProduct * sizeof(WCHAR));

	DecompSize       = SenrenBankaInfo.OriginalSize;
	IndexBuffer      = (PBYTE)AllocateMemoryP(SenrenBankaInfo.OriginalSize);
	CompressedBuffer = (PBYTE)AllocateMemoryP(SenrenBankaInfo.ArchiveSize);

	if (!IndexBuffer || !CompressedBuffer)
	{
		MessageBoxW(Handle->MainWindow, L"Insufficient memory", L"KrkrExtract", MB_OK);

		if (IndexBuffer)      FreeMemoryP(IndexBuffer);
		if (CompressedBuffer) FreeMemoryP(CompressedBuffer);
		return STATUS_INSUFFICIENT_RESOURCES;
	}

	file.Seek(SenrenBankaInfo.Offset, FILE_BEGIN);
	Status = file.Read(CompressedBuffer, SenrenBankaInfo.ArchiveSize);
	if (NT_FAILED(Status))
	{
		FreeMemoryP(IndexBuffer);
		FreeMemoryP(CompressedBuffer);

		MessageBoxW(Handle->MainWindow, L"Couldn't decompress the special block", L"KrkrExtract", MB_OK | MB_ICONERROR);
		return Status;
	}

	if ((DecompSize = uncompress((PBYTE)IndexBuffer, (PULONG)&DecompSize,
		(PBYTE)CompressedBuffer, SenrenBankaInfo.ArchiveSize)) != Z_OK)
	{
		if (Handle->DebugOn)
			PrintConsoleW(L"Failed to gather information(at Compressed block)\n");

		MessageBoxW(NULL, L"Failed to decompress special chunk", L"KrkrExtract", MB_OK);
		return STATUS_DATA_ERROR;
	}

	M2SubChunkMagic = *(PDWORD)IndexBuffer;

	FreeMemoryP(IndexBuffer);
	FreeMemoryP(CompressedBuffer);

	return STATUS_SUCCESS;
}


NTSTATUS NTAPI KrkrPacker::DetactPackFormat(LPCWSTR lpFileName)
{
	NTSTATUS                Status;
	ULONG                   Count;
	GlobalData*             Handle;
	NtFileDisk              File;
	KRKR2_XP3_HEADER        XP3Header;
	KRKR2_XP3_DATA_HEADER   DataHeader;
	PBYTE                   Indexdata;
	LARGE_INTEGER           BeginOffset;
	ULONG                   OldHash, NewHash;
	IStream*                Stream;
	STATSTG                 Stat;

	Count = 0;
	Handle = GlobalData::GetGlobalData();

	Status = File.Open(lpFileName);
	if (NT_FAILED(Status))
	{
		wstring Info(L"Couldn't open : \n");
		Info += lpFileName;
		Info += L"\nFor Guessing XP3 Package Type!";
		MessageBoxW(NULL, Info.c_str(), L"KrkrExtract", MB_OK);
		return Status;
	}

	BeginOffset.QuadPart = 0;

	File.Read((PBYTE)(&XP3Header), sizeof(XP3Header));

	//Exe Built-in Package Support
	if ((*(PUSHORT)XP3Header.Magic) == IMAGE_DOS_SIGNATURE)
	{
		Status = FindEmbededXp3OffsetSlow(lpFileName, &BeginOffset);

		if (NT_FAILED(Status))
		{
			MessageBoxW(NULL, L"No a Built-in Package\n", L"KrkrExtract", MB_OK);
			File.Close();
			return Status;
		}

		File.Seek(BeginOffset, FILE_BEGIN);
		File.Read((PBYTE)(&XP3Header), sizeof(XP3Header));
	}
	else
	{
		BeginOffset.QuadPart = 0;
	}

	if (RtlCompareMemory(StaticXP3V2Magic, XP3Header.Magic, sizeof(StaticXP3V2Magic)) != sizeof(StaticXP3V2Magic))
	{
		MessageBoxW(NULL, L"No a XP3 Package!", L"KrkrExtract", MB_OK);
		File.Close();
		return STATUS_INVALID_PARAMETER;
	}

	ULONG64 CompresseBufferSize = 0x1000;
	ULONG64 DecompressBufferSize = 0x1000;
	PBYTE pCompress = (PBYTE)AllocateMemoryP((ULONG)CompresseBufferSize);
	PBYTE pDecompress = (PBYTE)AllocateMemoryP((ULONG)DecompressBufferSize);
	DataHeader.OriginalSize = XP3Header.IndexOffset;


	ULONG Result = PackInfo::UnknownPack;
	do
	{
		LARGE_INTEGER Offset;

		Offset.QuadPart = DataHeader.OriginalSize.QuadPart + BeginOffset.QuadPart;
		File.Seek(Offset.LowPart, FILE_BEGIN);
		Status = File.Read((PBYTE)(&DataHeader), sizeof(DataHeader));
		if (NT_FAILED(Status))
		{
			MessageBoxW(NULL, L"Couldn't Read Index Header", L"KrkrExtract", MB_OK);
			FreeMemoryP(pCompress);
			FreeMemoryP(pDecompress);
			File.Close();
			return STATUS_UNSUCCESSFUL;
		}

		if (DataHeader.ArchiveSize.HighPart != 0 || DataHeader.ArchiveSize.LowPart == 0)
			continue;

		if (DataHeader.ArchiveSize.LowPart > CompresseBufferSize)
		{
			CompresseBufferSize = DataHeader.ArchiveSize.LowPart;
			pCompress = (PBYTE)ReAllocateMemoryP(pCompress, (ULONG)CompresseBufferSize);
		}

		if ((DataHeader.bZlib & 7) == 0)
		{
			Offset.QuadPart = -8;
			File.Seek(Offset.LowPart, FILE_CURRENT);
		}

		File.Read(pCompress, DataHeader.ArchiveSize.LowPart);
		BOOL EncodeMark = DataHeader.bZlib & 7;

		if (EncodeMark == FALSE)
		{
			if (DataHeader.ArchiveSize.LowPart > DecompressBufferSize)
			{
				DecompressBufferSize = DataHeader.ArchiveSize.LowPart;
				pDecompress = (PBYTE)ReAllocateMemoryP(pDecompress, (ULONG)DecompressBufferSize);
			}
			CopyMemory(pDecompress, pCompress, DataHeader.ArchiveSize.LowPart);
			DataHeader.OriginalSize.LowPart = DataHeader.ArchiveSize.LowPart;
		}
		else
		{
			if (DataHeader.OriginalSize.LowPart > DecompressBufferSize)
			{
				DecompressBufferSize = DataHeader.OriginalSize.LowPart;
				pDecompress = (PBYTE)ReAllocateMemoryP(pDecompress, (ULONG)DecompressBufferSize);
			}

			DataHeader.OriginalSize.HighPart = DataHeader.OriginalSize.LowPart;
			if (uncompress((PBYTE)pDecompress, (PULONG)&DataHeader.OriginalSize.HighPart,
				(PBYTE)pCompress, DataHeader.ArchiveSize.LowPart) == Z_OK)
			{
				DataHeader.OriginalSize.LowPart = DataHeader.OriginalSize.HighPart;
			}
		}

		if (IsCompatXP3(pDecompress, DataHeader.OriginalSize.LowPart, &Handle->M2ChunkMagic))
		{
			KrkrPackType = FindChunkMagicFirst(pDecompress, DataHeader.OriginalSize.LowPart);
			switch (DetectCompressedChunk(pDecompress, DataHeader.OriginalSize.LowPart))
			{
			case TRUE:
				Result = InitIndexFileFirst(pDecompress, DataHeader.OriginalSize.LowPart);
				break;

			case FALSE:
				Result = InitIndexFile_SenrenBanka(pDecompress, DataHeader.OriginalSize.LowPart, File);
				GetSenrenBankaPackInfo(pDecompress, DataHeader.OriginalSize.LowPart, File);
				KrkrPackType = PackInfo::KrkrZ_SenrenBanka;
				break;
			}
		}
		else
		{
			Result = InitIndex_NekoVol0(pDecompress, DataHeader.OriginalSize.LowPart);
			KrkrPackType = PackInfo::KrkrZ_V2;
		}

		if (KrkrPackType == PackInfo::UnknownPack)
		{
			FreeMemoryP(pCompress);
			FreeMemoryP(pDecompress);
			File.Close();

			MessageBoxW(NULL, L"Unknown Pack Type", L"KrkrExtract", MB_OK | MB_ICONERROR);
			return STATUS_UNSUCCESSFUL;
		}

	} while (DataHeader.bZlib & 0x80);

	//
	if (KrkrPackType == PackInfo::NormalPack && Handle->pfGlobalXP3Filter == NULL)
	{
		Handle->ItemVector[Handle->ItemVector.size() / 2].info.FileName;
		Stream = TVPCreateIStream(ttstr(Handle->ItemVector[Handle->ItemVector.size() / 2].info.FileName.c_str()), TJS_BS_READ);
		OldHash = Handle->ItemVector[Handle->ItemVector.size() / 2].adlr.Hash;
		if (Stream)
		{
			Stream->Stat(&Stat, STATFLAG_DEFAULT);
			NewHash = adler32IStream(0, Stream, Stat.cbSize);

			if (OldHash != NewHash)
				KrkrPackType = PackInfo::NormalPack_NoExporter;
		}
	}

	FreeMemoryP(pCompress);
	FreeMemoryP(pDecompress);
	File.Close();

	return STATUS_SUCCESS;;
}


Void WINAPI KrkrPacker::InternalReset()
{
	GlobalData*   Handle;

	Handle = GlobalData::GetGlobalData();

	RtlZeroMemory(&SenrenBankaInfo, sizeof(SenrenBankaInfo));

	PackChunkList.clear();
	M2ChunkList.clear();
	FileList.clear();
	Handle->Reset();
}


NTSTATUS WINAPI KrkrPacker::DoNormalPack(LPCWSTR lpBasePack, LPCWSTR lpGuessPack, LPCWSTR OutName)
{
	NTSTATUS                Status;
	BOOL                    Result;
	NtFileDisk              File, FileXP3;
	GlobalData*             Handle;
	PBYTE                   pbIndex;
	ULONG                   BufferSize, CompressedSize;
	PVOID                   lpBuffer, lpCompressBuffer;
	LARGE_INTEGER           Size, Offset, BytesTransfered;
	SMyXP3IndexNormal       *pXP3Index, *pIndex;
	KRKR2_XP3_DATA_HEADER   IndexHeader;
	BYTE                    FirstMagic[11] = { 0x58, 0x50, 0x33, 0x0D, 0x0A, 0x20, 0x0A, 0x1A, 0x8B, 0x67, 0x01 };
	KRKR2_XP3_HEADER        XP3Header(FirstMagic, 0);
	wstring                 WStrBasePath(lpBasePack);

	FileList.clear();
	IterFiles(lpBasePack);

	Handle = GlobalData::GetGlobalData();

	Status = FileXP3.Create(OutName);
	if (NT_FAILED(Status))
	{
		MessageBoxW(NULL, L"Couldn't open a handle for temporary output file.", L"KrkrExtract", MB_OK);
		return Status;
	}

	BufferSize = 0x10000;
	CompressedSize = BufferSize;
	lpBuffer = AllocateMemoryP(BufferSize);
	lpCompressBuffer = AllocateMemoryP(CompressedSize);
	pXP3Index = (SMyXP3IndexNormal *)AllocateMemoryP(sizeof(*pXP3Index) * FileList.size());
	pIndex = pXP3Index;

	if (!lpBuffer || !lpCompressBuffer || !pXP3Index)
	{
		MessageBoxW(Handle->MainWindow, L"Insufficient memory to make package!!", L"KrkrExtract", MB_OK | MB_ICONERROR);
		FileXP3.Close();
		
		if (lpBuffer)         FreeMemoryP(lpBuffer);
		if (lpCompressBuffer) FreeMemoryP(lpCompressBuffer);
		if (pXP3Index)        FreeMemoryP(pXP3Index);
		return STATUS_INSUFFICIENT_RESOURCES;
	}

	FileXP3.Write(&XP3Header, sizeof(XP3Header), &BytesTransfered);

	Offset.QuadPart = BytesTransfered.QuadPart;
	for (ULONG i = 0; i < FileList.size(); ++pIndex, i++)
	{
		WCHAR OutInfo[MAX_PATH];
		FormatStringW(OutInfo, PackingFormatString, i + 1, FileList.size());
		SetWindowTextW(Handle->MainWindow, OutInfo);
		Handle->SetProcess(Handle->MainWindow, (ULONG)(((float)(i + 1) / (float)FileList.size()) * 100.0));

		ZeroMemory(pIndex, sizeof(*pIndex));
		*(PDWORD)(pIndex->file.Magic) = CHUNK_MAGIC_FILE;
		*(PDWORD)(pIndex->info.Magic) = CHUNK_MAGIC_INFO;
		*(PDWORD)(pIndex->time.Magic) = CHUNK_MAGIC_TIME;
		*(PDWORD)(pIndex->segm.Magic) = CHUNK_MAGIC_SEGM;
		*(PDWORD)(pIndex->adlr.Magic) = CHUNK_MAGIC_ADLR;
		pIndex->segm.ChunkSize.QuadPart = (ULONG64)0x1C; //sizeof(pIndex->segm.segm);
		pIndex->adlr.ChunkSize.QuadPart = (ULONG64)0x04;
		pIndex->info.ChunkSize.QuadPart = (ULONG64)0x58;

		//
		pIndex->file.ChunkSize.QuadPart = (ULONG64)0xB0;
		pIndex->time.ChunkSize.QuadPart = (ULONG64)0x08;

		wstring FullName = wstring(lpBasePack) + L"\\";
		FullName += FileList[i].c_str();

		Status = File.Open(FullName.c_str());
		if (NT_FAILED(Status))
		{
			if (Handle->DebugOn)
				PrintConsoleW(L"Couldn't open %s\n", FullName.c_str());
		}

		File.GetSize(&Size);
		if (Size.LowPart > BufferSize)
		{
			BufferSize = Size.LowPart;
			lpBuffer = ReAllocateMemoryP(lpBuffer, BufferSize);
		}

		File.Read(lpBuffer, Size.LowPart, &BytesTransfered);
		
		if (BytesTransfered.LowPart != Size.LowPart)
		{
			wstring InfoW(L"Dummy write :\n[Failed to read]\n");
			InfoW += FullName;
			MessageBoxW(Handle->MainWindow, InfoW.c_str(), L"KrkrExtract", MB_OK);
			File.Close();
			FileXP3.Close();
			FreeMemoryP(lpBuffer);
			FreeMemoryP(lpCompressBuffer);
			FreeMemoryP(pXP3Index);
			return STATUS_IO_DEVICE_ERROR;
		}

		pIndex->segm.segm->Offset = Offset;

		pIndex->info.FileName = FileList[i].c_str();
		pIndex->info.FileNameLength = FileList[i].length();

		pIndex->adlr.Hash = adler32(1, (Bytef *)lpBuffer, BytesTransfered.LowPart);

		pIndex->segm.segm->OriginalSize.LowPart = BytesTransfered.LowPart;
		pIndex->info.OriginalSize.LowPart = BytesTransfered.LowPart;

		FILETIME Time1, Time2;
		GetFileTime(File.GetHandle(), &(pIndex->time.FileTime), &Time1, &Time2);
		File.Close();


		LARGE_INTEGER EncryptOffset;

		EncryptOffset.QuadPart = 0;
		DecryptWorker(EncryptOffset.LowPart, (PBYTE)lpBuffer, BytesTransfered.LowPart, pIndex->adlr.Hash);
		if (XP3EncryptionFlag && pfProc)
			pIndex->info.EncryptedFlag = 0x80000000;
		else
			pIndex->info.EncryptedFlag = 0;

		pIndex->file.ChunkSize.QuadPart = (ULONG64)sizeof(pIndex->time.ChunkSize) + MagicLength + sizeof(pIndex->time.FileTime) +
			sizeof(pIndex->info.ArchiveSize) * 2 + sizeof(pIndex->info.EncryptedFlag) + sizeof(USHORT) + sizeof(pIndex->info.ChunkSize) + MagicLength + (pIndex->info.FileName.length() + 1) * 2 +
			sizeof(pIndex->segm.ChunkSize) + MagicLength + sizeof(pIndex->segm.segm[0].ArchiveSize) * 3 + sizeof(BOOL) +
			sizeof(pIndex->adlr.ChunkSize) + MagicLength + sizeof(pIndex->adlr.Hash);

		if (!InfoNameZeroEnd)
			pIndex->file.ChunkSize.QuadPart -= 2;

		pIndex->segm.segm->bZlib = 0;

		pIndex->segm.segm->ArchiveSize.LowPart = BytesTransfered.LowPart;
		pIndex->info.ArchiveSize.LowPart = BytesTransfered.LowPart;
		Offset.QuadPart += BytesTransfered.QuadPart;

		FileXP3.Write(lpBuffer, BytesTransfered.LowPart, &BytesTransfered);
	}

	XP3Header.IndexOffset = Offset;

	// generate index, calculate index size first
	Size.LowPart = 0;
	pIndex = pXP3Index;

	for (ULONG i = 0; i < FileList.size(); ++i, ++pIndex)
	{
		Size.LowPart +=
			sizeof(pIndex->time.ChunkSize) + MagicLength + sizeof(pIndex->time.FileTime) +
			sizeof(pIndex->file.ChunkSize) + MagicLength +
			sizeof(pIndex->info.ArchiveSize) * 2 + sizeof(pIndex->info.EncryptedFlag) + sizeof(USHORT) + sizeof(pIndex->info.ChunkSize) + MagicLength + (pIndex->info.FileName.length() + 1) * 2 +
			sizeof(pIndex->segm.ChunkSize) + MagicLength + sizeof(pIndex->segm.segm[0].ArchiveSize) * 3 + sizeof(BOOL) +
			sizeof(pIndex->adlr.ChunkSize) + MagicLength + sizeof(pIndex->adlr.Hash);

		if (!InfoNameZeroEnd)
			Size.LowPart -= 2;
	}

	if (Size.LowPart > CompressedSize)
	{
		CompressedSize = Size.LowPart;
		lpCompressBuffer = ReAllocateMemoryP(lpCompressBuffer, CompressedSize);
	}
	if (Size.LowPart * 2 > BufferSize)
	{
		BufferSize = Size.LowPart * 2;
		lpBuffer = ReAllocateMemoryP(lpBuffer, BufferSize);
	}

	// generate index to lpCompressBuffer
	pIndex = pXP3Index;
	pbIndex = (PBYTE)lpCompressBuffer;
	for (ULONG i = 0; i < FileList.size(); ++pIndex, i++)
	{
		DWORD n = sizeof(DWORD);
		CopyMemory(pbIndex, pIndex->file.Magic, n);
		pbIndex += n;
		n = 8;
		CopyMemory(pbIndex, &(pIndex->file.ChunkSize), n);
		pbIndex += n;

		n = sizeof(DWORD);
		CopyMemory(pbIndex, pIndex->time.Magic, n);
		pbIndex += n;
		n = 8;
		CopyMemory(pbIndex, &(pIndex->time.ChunkSize), n);
		pbIndex += n;
		n = sizeof(pIndex->time.FileTime);
		CopyMemory(pbIndex, &(pIndex->time.FileTime), n);
		pbIndex += n;

		n = sizeof(DWORD);
		CopyMemory(pbIndex, pIndex->adlr.Magic, n);
		pbIndex += n;
		n = 8;
		CopyMemory(pbIndex, &(pIndex->adlr.ChunkSize), n);
		pbIndex += n;
		n = 4;
		CopyMemory(pbIndex, &(pIndex->adlr.Hash), n);
		pbIndex += n;

		n = 4;
		CopyMemory(pbIndex, &(pIndex->segm.Magic), n);
		pbIndex += n;
		n = 8;
		CopyMemory(pbIndex, &(pIndex->segm.ChunkSize), n);
		pbIndex += n;
		n = 4;
		CopyMemory(pbIndex, &(pIndex->segm.segm[0].bZlib), n);
		pbIndex += n;
		n = 8;
		CopyMemory(pbIndex, &(pIndex->segm.segm[0].Offset), n);
		pbIndex += n;
		CopyMemory(pbIndex, &(pIndex->segm.segm[0].OriginalSize), n);
		pbIndex += n;
		CopyMemory(pbIndex, &(pIndex->segm.segm[0].ArchiveSize), n);
		pbIndex += n;

		n = 4;
		CopyMemory(pbIndex, &(pIndex->info.Magic), n);
		pbIndex += n;
		n = 8;
		CopyMemory(pbIndex, &(pIndex->info.ChunkSize), n);
		pbIndex += n;
		n = 4;
		CopyMemory(pbIndex, &(pIndex->info.EncryptedFlag), n);
		pbIndex += n;
		n = 8;
		CopyMemory(pbIndex, &(pIndex->info.OriginalSize), n);
		pbIndex += n;
		CopyMemory(pbIndex, &(pIndex->info.ArchiveSize), n);
		pbIndex += n;
		n = 2;
		CopyMemory(pbIndex, &(pIndex->info.FileNameLength), n);
		pbIndex += n;

		if (InfoNameZeroEnd)
		{
			n = (pIndex->info.FileName.length() + 1) * 2;
			CopyMemory(pbIndex, pIndex->info.FileName.c_str(), n);
		}
		else
		{
			n = (pIndex->info.FileName.length()) * 2;
			CopyMemory(pbIndex, pIndex->info.FileName.c_str(), n);
		}
		pbIndex += n;
	}

	IndexHeader.bZlib = 1;
	IndexHeader.OriginalSize.QuadPart = Size.LowPart;
	IndexHeader.ArchiveSize.LowPart = BufferSize;
	BufferSize = Size.LowPart;
	compress2((PBYTE)lpBuffer, &IndexHeader.ArchiveSize.LowPart, (PBYTE)lpCompressBuffer, BufferSize, Z_BEST_COMPRESSION);
	IndexHeader.ArchiveSize.HighPart = 0;

	FileXP3.Write(&IndexHeader, sizeof(IndexHeader), &BytesTransfered);
	FileXP3.Write(lpBuffer, IndexHeader.ArchiveSize.LowPart, &BytesTransfered);
	Offset.QuadPart = 0;
	FileXP3.Seek(Offset, FILE_BEGIN);
	FileXP3.Write(&XP3Header, sizeof(XP3Header), &BytesTransfered);

	FileXP3.Close();

	FreeMemoryP(lpBuffer);
	FreeMemoryP(lpCompressBuffer);
	FreeMemoryP(pXP3Index);

	MessageBoxW(Handle->MainWindow, L"Making Package : Successful", L"KrkrExtract", MB_OK);
	return STATUS_SUCCESS;
}


NTSTATUS NTAPI KrkrPacker::DoNormalPackEx(LPCWSTR lpBasePack, LPCWSTR GuessPackage, LPCWSTR OutName)
{
	NTSTATUS                Status;
	BOOL                    Result;
	NtFileDisk              File, FileXP3;
	GlobalData*             Handle;
	PBYTE                   pbIndex;
	ULONG                   BufferSize, CompressedSize, StrLen;
	PVOID                   lpBuffer, lpCompressBuffer;
	LARGE_INTEGER           Size, Offset, BytesTransfered;
	SMyXP3IndexNormal       *pXP3Index, *pIndex;
	KRKR2_XP3_DATA_HEADER   IndexHeader;
	BYTE                    FirstMagic[11] = { 0x58, 0x50, 0x33, 0x0D, 0x0A, 0x20, 0x0A, 0x1A, 0x8B, 0x67, 0x01 };
	KRKR2_XP3_HEADER        XP3Header(FirstMagic, 0);
	wstring                 WStrBasePath(lpBasePack);

	FileList.clear();
	IterFiles(lpBasePack);

	Handle = GlobalData::GetGlobalData();

	Status = DoDummyNormalPackExFirst(lpBasePack);
	if (NT_FAILED(Status))
		return Status;

	if (Handle->DebugOn)
		PrintConsoleW(L"Packing files...\n");

	TVPExecuteScript(ttstr(L"Storages.addAutoPath(System.exePath + \"" + ttstr(Handle->CurrentTempFileName.c_str()) + L"\" + \">\");"));

	Status = FileXP3.Create(OutName);
	if (NT_FAILED(Status))
	{
		MessageBoxW(NULL, L"Couldn't create a handle for output file.", L"KrkrExtract", MB_OK);
		return Status;
	}

	BufferSize = 0x10000;
	CompressedSize = BufferSize;
	lpBuffer = AllocateMemoryP(BufferSize);
	lpCompressBuffer = AllocateMemoryP(CompressedSize);
	pXP3Index = (SMyXP3IndexNormal *)AllocateMemoryP(sizeof(*pXP3Index) * FileList.size());
	pIndex = pXP3Index;

	if (!lpBuffer || !lpCompressBuffer || !pXP3Index)
	{
		MessageBoxW(Handle->MainWindow, L"Insufficient memory to make package!!", L"KrkrExtract", MB_OK | MB_ICONERROR);
		FileXP3.Close();

		if (lpBuffer)         FreeMemoryP(lpBuffer);
		if (lpCompressBuffer) FreeMemoryP(lpCompressBuffer);
		if (pXP3Index)        FreeMemoryP(pXP3Index);
		return STATUS_INSUFFICIENT_RESOURCES;
	}

	FileXP3.Write(&XP3Header, sizeof(XP3Header), &BytesTransfered);

	Offset.QuadPart = BytesTransfered.QuadPart;
	for (ULONG i = 0; i < FileList.size(); ++pIndex, i++)
	{
		WCHAR OutInfo[MAX_PATH];
		FormatStringW(OutInfo, PackingFormatString, i + 1, FileList.size());
		SetWindowTextW(Handle->MainWindow, OutInfo);
		Handle->SetProcess(Handle->MainWindow, (ULONG)(((float)(i + 1) / (float)FileList.size()) * 100.0));

		ZeroMemory(pIndex, sizeof(*pIndex));
		*(PDWORD)(pIndex->file.Magic) = CHUNK_MAGIC_FILE;
		*(PDWORD)(pIndex->info.Magic) = CHUNK_MAGIC_INFO;
		*(PDWORD)(pIndex->time.Magic) = CHUNK_MAGIC_TIME;
		*(PDWORD)(pIndex->segm.Magic) = CHUNK_MAGIC_SEGM;
		*(PDWORD)(pIndex->adlr.Magic) = CHUNK_MAGIC_ADLR;
		pIndex->segm.ChunkSize.QuadPart = (ULONG64)0x1C; //sizeof(pIndex->segm.segm);
		pIndex->adlr.ChunkSize.QuadPart = (ULONG64)0x04;
		pIndex->info.ChunkSize.QuadPart = (ULONG64)0x58;

		//
		pIndex->file.ChunkSize.QuadPart = (ULONG64)0xB0;
		pIndex->time.ChunkSize.QuadPart = (ULONG64)0x08;

		wstring DummyName = FileList[i] + L".dummy";

		ttstr FullName(L"file://./");

		StrLen = StrLengthW(lpBasePack);

		for (ULONG Index = 0; Index < StrLen; Index++)
		{
			if (lpBasePack[Index] == L'\\' || lpBasePack[Index] == L'/')
				FullName += L"/";
			else if (lpBasePack[Index] == L':')
			{
				i++;
				FullName += CHAR_LOWER(lpBasePack[Index]);
			}
			else
				FullName += lpBasePack[Index];
		}

		if (lpBasePack[StrLen - 1] != L'\\' && lpBasePack[StrLen - 1] != L'/')
			FullName += L"/";

		FullName += Handle->CurrentTempFileName.c_str();
		FullName += L"/";

		FullName += DummyName.c_str();

		IStream* Stream = TVPCreateIStream(FullName, TJS_BS_READ);
		if (Stream == NULL)
		{
			if (Handle->DebugOn)
				PrintConsoleW(L"Couldn't open %s\n", DummyName.c_str());

			wstring InfoW(L"Couldn't open :\n");
			InfoW += FileList[i];
			MessageBoxW(Handle->MainWindow, InfoW.c_str(), L"KrkrExtract", MB_OK);
			FileXP3.Close();
			FreeMemoryP(lpBuffer);
			FreeMemoryP(lpCompressBuffer);
			FreeMemoryP(pXP3Index);
			return STATUS_UNSUCCESSFUL;
		}


		STATSTG t;
		Stream->Stat(&t, STATFLAG_DEFAULT);

		Size.QuadPart = t.cbSize.QuadPart;
		if (Size.LowPart > BufferSize)
		{
			BufferSize = Size.LowPart;
			lpBuffer = ReAllocateMemoryP(lpBuffer, BufferSize);
		}
		BytesTransfered.QuadPart = 0;
		Stream->Read(lpBuffer, Size.LowPart, &BytesTransfered.LowPart);

		if (BytesTransfered.LowPart != Size.LowPart)
		{
			wstring InfoW(L"Couldn't read :\n");
			InfoW += FileList[i];
			MessageBoxW(Handle->MainWindow, InfoW.c_str(), L"KrkrExtract", MB_OK);
			FileXP3.Close();
			FreeMemoryP(lpBuffer);
			FreeMemoryP(lpCompressBuffer);
			FreeMemoryP(pXP3Index);
			return STATUS_UNSUCCESSFUL;
		}

		pIndex->segm.segm->Offset = Offset;

		pIndex->info.FileName = FileList[i];
		pIndex->info.FileNameLength = FileList[i].length();

		pIndex->adlr.Hash = adler32(1, (Bytef *)lpBuffer, BytesTransfered.LowPart);

		pIndex->segm.segm->OriginalSize.LowPart = BytesTransfered.LowPart;
		pIndex->info.OriginalSize.LowPart = BytesTransfered.LowPart;

		FILETIME Time1, Time2;
		GetFileTime(File.GetHandle(), &(pIndex->time.FileTime), &Time1, &Time2);
		File.Close();


		LARGE_INTEGER EncryptOffset;

		EncryptOffset.QuadPart = 0;
		DecryptWorker(EncryptOffset.LowPart, (PBYTE)lpBuffer, BytesTransfered.LowPart, pIndex->adlr.Hash);
		if (XP3EncryptionFlag && pfProc)
			pIndex->info.EncryptedFlag = 0x80000000;
		else
			pIndex->info.EncryptedFlag = 0;

		pIndex->file.ChunkSize.QuadPart = (ULONG64)sizeof(pIndex->time.ChunkSize) + MagicLength + sizeof(pIndex->time.FileTime) +
			sizeof(pIndex->info.ArchiveSize) * 2 + sizeof(pIndex->info.EncryptedFlag) + sizeof(USHORT) + sizeof(pIndex->info.ChunkSize) + MagicLength + (pIndex->info.FileName.length() + 1) * 2 +
			sizeof(pIndex->segm.ChunkSize) + MagicLength + sizeof(pIndex->segm.segm[0].ArchiveSize) * 3 + sizeof(BOOL) +
			sizeof(pIndex->adlr.ChunkSize) + MagicLength + sizeof(pIndex->adlr.Hash);

		if (!InfoNameZeroEnd)
			pIndex->file.ChunkSize.QuadPart -= 2;

		pIndex->segm.segm->bZlib = 0;

		pIndex->segm.segm->ArchiveSize.LowPart = BytesTransfered.LowPart;
		pIndex->info.ArchiveSize.LowPart = BytesTransfered.LowPart;
		Offset.QuadPart += BytesTransfered.QuadPart;

		FileXP3.Write(lpBuffer, BytesTransfered.LowPart, &BytesTransfered);
	}

	XP3Header.IndexOffset = Offset;

	// generate index, calculate index size first
	Size.LowPart = 0;
	pIndex = pXP3Index;

	for (ULONG i = 0; i < FileList.size(); ++i, ++pIndex)
	{
		Size.LowPart +=
			sizeof(pIndex->time.ChunkSize) + MagicLength + sizeof(pIndex->time.FileTime) +
			sizeof(pIndex->file.ChunkSize) + MagicLength +
			sizeof(pIndex->info.ArchiveSize) * 2 + sizeof(pIndex->info.EncryptedFlag) + sizeof(USHORT) + sizeof(pIndex->info.ChunkSize) + MagicLength + (pIndex->info.FileName.length() + 1) * 2 +
			sizeof(pIndex->segm.ChunkSize) + MagicLength + sizeof(pIndex->segm.segm[0].ArchiveSize) * 3 + sizeof(BOOL) +
			sizeof(pIndex->adlr.ChunkSize) + MagicLength + sizeof(pIndex->adlr.Hash);

		if (!InfoNameZeroEnd)
			Size.LowPart -= 2;
	}

	if (Size.LowPart > CompressedSize)
	{
		CompressedSize = Size.LowPart;
		lpCompressBuffer = ReAllocateMemoryP(lpCompressBuffer, CompressedSize);
	}
	if (Size.LowPart * 2 > BufferSize)
	{
		BufferSize = Size.LowPart * 2;
		lpBuffer = ReAllocateMemoryP(lpBuffer, BufferSize);
	}

	// generate index to lpCompressBuffer
	pIndex = pXP3Index;
	pbIndex = (PBYTE)lpCompressBuffer;
	for (ULONG i = 0; i < FileList.size(); ++pIndex, i++)
	{
		DWORD n = sizeof(DWORD);
		CopyMemory(pbIndex, pIndex->file.Magic, n);
		pbIndex += n;
		n = 8;
		CopyMemory(pbIndex, &(pIndex->file.ChunkSize), n);
		pbIndex += n;

		n = sizeof(DWORD);
		CopyMemory(pbIndex, pIndex->time.Magic, n);
		pbIndex += n;
		n = 8;
		CopyMemory(pbIndex, &(pIndex->time.ChunkSize), n);
		pbIndex += n;
		n = sizeof(pIndex->time.FileTime);
		CopyMemory(pbIndex, &(pIndex->time.FileTime), n);
		pbIndex += n;

		n = sizeof(DWORD);
		CopyMemory(pbIndex, pIndex->adlr.Magic, n);
		pbIndex += n;
		n = 8;
		CopyMemory(pbIndex, &(pIndex->adlr.ChunkSize), n);
		pbIndex += n;
		n = 4;
		CopyMemory(pbIndex, &(pIndex->adlr.Hash), n);
		pbIndex += n;

		n = 4;
		CopyMemory(pbIndex, &(pIndex->segm.Magic), n);
		pbIndex += n;
		n = 8;
		CopyMemory(pbIndex, &(pIndex->segm.ChunkSize), n);
		pbIndex += n;
		n = 4;
		CopyMemory(pbIndex, &(pIndex->segm.segm[0].bZlib), n);
		pbIndex += n;
		n = 8;
		CopyMemory(pbIndex, &(pIndex->segm.segm[0].Offset), n);
		pbIndex += n;
		CopyMemory(pbIndex, &(pIndex->segm.segm[0].OriginalSize), n);
		pbIndex += n;
		CopyMemory(pbIndex, &(pIndex->segm.segm[0].ArchiveSize), n);
		pbIndex += n;

		n = 4;
		CopyMemory(pbIndex, &(pIndex->info.Magic), n);
		pbIndex += n;
		n = 8;
		CopyMemory(pbIndex, &(pIndex->info.ChunkSize), n);
		pbIndex += n;
		n = 4;
		CopyMemory(pbIndex, &(pIndex->info.EncryptedFlag), n);
		pbIndex += n;
		n = 8;
		CopyMemory(pbIndex, &(pIndex->info.OriginalSize), n);
		pbIndex += n;
		CopyMemory(pbIndex, &(pIndex->info.ArchiveSize), n);
		pbIndex += n;
		n = 2;
		CopyMemory(pbIndex, &(pIndex->info.FileNameLength), n);
		pbIndex += n;

		if (InfoNameZeroEnd)
		{
			n = (pIndex->info.FileName.length() + 1) * 2;
			CopyMemory(pbIndex, pIndex->info.FileName.c_str(), n);
		}
		else
		{
			n = (pIndex->info.FileName.length()) * 2;
			CopyMemory(pbIndex, pIndex->info.FileName.c_str(), n);
		}
		pbIndex += n;
	}

	IndexHeader.bZlib = 1;
	IndexHeader.OriginalSize.QuadPart = Size.LowPart;
	IndexHeader.ArchiveSize.LowPart = BufferSize;
	BufferSize = Size.LowPart;
	compress2((PBYTE)lpBuffer, &IndexHeader.ArchiveSize.LowPart, (PBYTE)lpCompressBuffer, BufferSize, Z_BEST_COMPRESSION);
	IndexHeader.ArchiveSize.HighPart = 0;

	FileXP3.Write(&IndexHeader, sizeof(IndexHeader), &BytesTransfered);
	FileXP3.Write(lpBuffer, IndexHeader.ArchiveSize.LowPart, &BytesTransfered);
	Offset.QuadPart = 0;
	FileXP3.Seek(Offset, FILE_BEGIN);
	FileXP3.Write(&XP3Header, sizeof(XP3Header), &BytesTransfered);

	FileXP3.Close();

	FreeMemoryP(lpBuffer);
	FreeMemoryP(lpCompressBuffer);
	FreeMemoryP(pXP3Index);

	TVPExecuteScript(ttstr(L"Storages.removeAutoPath(System.exePath + \"" +  ttstr(Handle->CurrentTempFileName.c_str()) + L"\" + \">\");"));

	CloseHandle(Handle->CurrentTempHandle);
	InterlockedExchangePointer(&(Handle->CurrentTempHandle), INVALID_HANDLE_VALUE);

	Status = Io::DeleteFileW(Handle->CurrentTempFileName.c_str());
	if (NT_FAILED(Status))
	{
		MessageBoxW(Handle->MainWindow, (L"Making Package : Successful!\nBut you must relaunch this game\nand delete \"" + Handle->CurrentTempFileName + L"\" to make the next package!!!").c_str(),
			L"KrkrExtract (Important Infomation!!)", MB_OK);
	}
	else
	{
		MessageBoxW(Handle->MainWindow, L"Making Package : Successful", L"KrkrExtract", MB_OK);
	}
	Handle->CurrentTempFileName = L"KrkrzTempWorker.xp3";
	return STATUS_SUCCESS;
}


NTSTATUS NTAPI KrkrPacker::DoDummyNormalPackExFirst(LPCWSTR lpBasePack)
{
	NTSTATUS                Status;
	BOOL                    Result;
	NtFileDisk              File, FileXP3;
	GlobalData*             Handle;
	PBYTE                   pbIndex;
	ULONG                   BufferSize, CompressedSize;
	PVOID                   lpBuffer, lpCompressBuffer;
	LARGE_INTEGER           Size, Offset, BytesTransfered;
	SMyXP3IndexNormal       *pXP3Index, *pIndex;
	KRKR2_XP3_DATA_HEADER   IndexHeader;
	BYTE                    FirstMagic[11] = { 0x58, 0x50, 0x33, 0x0D, 0x0A, 0x20, 0x0A, 0x1A, 0x8B, 0x67, 0x01 };
	KRKR2_XP3_HEADER        XP3Header(FirstMagic, 0);
	wstring                 WStrBasePath(lpBasePack);
	WCHAR                   CurTempFileName[MAX_PATH];
	ULONG64                 RandNum;

	Handle = GlobalData::GetGlobalData();

	RandNum = genrand64_int64();
	RtlZeroMemory(CurTempFileName, countof(CurTempFileName) * 2);
	FormatStringW(CurTempFileName, L"KrkrzTempWorker_%08x%08x.xp3", HIDWORD(RandNum), LODWORD(RandNum));
	Handle->CurrentTempFileName = CurTempFileName;

	Status = FileXP3.Create(Handle->CurrentTempFileName.c_str());
	if (NT_FAILED(Status))
	{
		MessageBoxW(NULL, L"Couldn't open a handle for temporary output file.", L"KrkrExtract", MB_OK);
		return Status;
	}

	BufferSize = 0x10000;
	CompressedSize = BufferSize;
	lpBuffer = AllocateMemoryP(BufferSize);
	lpCompressBuffer = AllocateMemoryP(CompressedSize);
	pXP3Index = (SMyXP3IndexNormal *)AllocateMemoryP(sizeof(*pXP3Index) * FileList.size());
	pIndex = pXP3Index;

	if (!lpBuffer || !lpCompressBuffer || !pXP3Index)
	{
		MessageBoxW(Handle->MainWindow, L"Insufficient memory to make package!!", L"KrkrExtract", MB_OK | MB_ICONERROR);
		FileXP3.Close();

		if (lpBuffer)         FreeMemoryP(lpBuffer);
		if (lpCompressBuffer) FreeMemoryP(lpCompressBuffer);
		if (pXP3Index)        FreeMemoryP(pXP3Index);
		return STATUS_INSUFFICIENT_RESOURCES;
	}

	FileXP3.Write(&XP3Header, sizeof(XP3Header), &BytesTransfered);

	Offset.QuadPart = BytesTransfered.QuadPart;
	for (ULONG i = 0; i < FileList.size(); ++pIndex, i++)
	{
		WCHAR OutInfo[MAX_PATH];
		FormatStringW(OutInfo, PackingFormatString, i + 1, FileList.size());
		SetWindowTextW(Handle->MainWindow, OutInfo);
		Handle->SetProcess(Handle->MainWindow, (ULONG)(((float)(i + 1) / (float)FileList.size()) * 100.0));

		ZeroMemory(pIndex, sizeof(*pIndex));
		*(PDWORD)(pIndex->file.Magic) = CHUNK_MAGIC_FILE;
		*(PDWORD)(pIndex->info.Magic) = CHUNK_MAGIC_INFO;
		*(PDWORD)(pIndex->time.Magic) = CHUNK_MAGIC_TIME;
		*(PDWORD)(pIndex->segm.Magic) = CHUNK_MAGIC_SEGM;
		*(PDWORD)(pIndex->adlr.Magic) = CHUNK_MAGIC_ADLR;
		pIndex->segm.ChunkSize.QuadPart = (ULONG64)0x1C; //sizeof(pIndex->segm.segm);
		pIndex->adlr.ChunkSize.QuadPart = (ULONG64)0x04;
		pIndex->info.ChunkSize.QuadPart = (ULONG64)0x58;

		//
		pIndex->file.ChunkSize.QuadPart = (ULONG64)0xB0;
		pIndex->time.ChunkSize.QuadPart = (ULONG64)0x08;

		wstring FullName = wstring(lpBasePack) + L"\\";
		FullName += FileList[i];

		Status = File.Open(FullName.c_str());
		if (NT_FAILED(Status))
		{
			if (Handle->DebugOn) 
				PrintConsoleW(L"Couldn't open %s\n", FullName.c_str());
		}

		File.GetSize(&Size);
		if (Size.LowPart > BufferSize)
		{
			BufferSize = Size.LowPart;
			lpBuffer = ReAllocateMemoryP(lpBuffer, BufferSize);
		}

		File.Read(lpBuffer, Size.LowPart, &BytesTransfered);

		if (BytesTransfered.LowPart != Size.LowPart)
		{
			wstring InfoW(L"Dummy write :\n[Failed to read]\n");
			InfoW += FullName;
			MessageBoxW(Handle->MainWindow, InfoW.c_str(), L"KrkrExtract", MB_OK);
			File.Close();
			FileXP3.Close();
			FreeMemoryP(lpBuffer);
			FreeMemoryP(lpCompressBuffer);
			FreeMemoryP(pXP3Index);
			return STATUS_IO_DEVICE_ERROR;
		}

		pIndex->segm.segm->Offset = Offset;

		pIndex->info.FileName = FileList[i] + L".dummy";
		pIndex->info.FileNameLength = (USHORT)(FileList[i].length() + StrLengthW(L".dummy"));

		pIndex->adlr.Hash = adler32(1, (Bytef *)lpBuffer, BytesTransfered.LowPart);

		pIndex->segm.segm->OriginalSize.LowPart = BytesTransfered.LowPart;
		pIndex->info.OriginalSize.LowPart = BytesTransfered.LowPart;

		FILETIME Time1, Time2;
		GetFileTime(File.GetHandle(), &(pIndex->time.FileTime), &Time1, &Time2);
		File.Close();


		LARGE_INTEGER EncryptOffset;

		EncryptOffset.QuadPart = 0;
		DecryptWorker(EncryptOffset.LowPart, (PBYTE)lpBuffer, BytesTransfered.LowPart, pIndex->adlr.Hash);
		if (XP3EncryptionFlag && pfProc)
			pIndex->info.EncryptedFlag = 0x80000000;
		else
			pIndex->info.EncryptedFlag = 0;

		pIndex->file.ChunkSize.QuadPart = (ULONG64)sizeof(pIndex->time.ChunkSize) + MagicLength + sizeof(pIndex->time.FileTime) +
			sizeof(pIndex->info.ArchiveSize) * 2 + sizeof(pIndex->info.EncryptedFlag) + sizeof(USHORT) + sizeof(pIndex->info.ChunkSize) + MagicLength + (pIndex->info.FileName.length() + 1) * 2 +
			sizeof(pIndex->segm.ChunkSize) + MagicLength + sizeof(pIndex->segm.segm[0].ArchiveSize) * 3 + sizeof(BOOL) +
			sizeof(pIndex->adlr.ChunkSize) + MagicLength + sizeof(pIndex->adlr.Hash);

		if (!InfoNameZeroEnd)
			pIndex->file.ChunkSize.QuadPart -= 2;

		pIndex->segm.segm->bZlib = 0;

		pIndex->segm.segm->ArchiveSize.LowPart = BytesTransfered.LowPart;
		pIndex->info.ArchiveSize.LowPart = BytesTransfered.LowPart;
		Offset.QuadPart += BytesTransfered.QuadPart;

		FileXP3.Write(lpBuffer, BytesTransfered.LowPart, &BytesTransfered);
	}

	XP3Header.IndexOffset = Offset;

	// generate index, calculate index size first
	Size.LowPart = 0;
	pIndex = pXP3Index;

	for (ULONG i = 0; i < FileList.size(); ++i, ++pIndex)
	{
		Size.LowPart +=
			sizeof(pIndex->time.ChunkSize) + MagicLength + sizeof(pIndex->time.FileTime) +
			sizeof(pIndex->file.ChunkSize) + MagicLength +
			sizeof(pIndex->info.ArchiveSize) * 2 + sizeof(pIndex->info.EncryptedFlag) + sizeof(USHORT) + sizeof(pIndex->info.ChunkSize) + MagicLength + (pIndex->info.FileName.length() + 1) * 2 +
			sizeof(pIndex->segm.ChunkSize) + MagicLength + sizeof(pIndex->segm.segm[0].ArchiveSize) * 3 + sizeof(BOOL) +
			sizeof(pIndex->adlr.ChunkSize) + MagicLength + sizeof(pIndex->adlr.Hash);

		if (!InfoNameZeroEnd)
			Size.LowPart -= 2;
	}

	if (Size.LowPart > CompressedSize)
	{
		CompressedSize = Size.LowPart;
		lpCompressBuffer = ReAllocateMemoryP(lpCompressBuffer, CompressedSize);
	}
	if (Size.LowPart * 2 > BufferSize)
	{
		BufferSize = Size.LowPart * 2;
		lpBuffer = ReAllocateMemoryP(lpBuffer, BufferSize);
	}

	// generate index to lpCompressBuffer
	pIndex = pXP3Index;
	pbIndex = (PBYTE)lpCompressBuffer;
	for (ULONG i = 0; i < FileList.size(); ++pIndex, i++)
	{
		DWORD n = sizeof(DWORD);
		CopyMemory(pbIndex, pIndex->file.Magic, n);
		pbIndex += n;
		n = 8;
		CopyMemory(pbIndex, &(pIndex->file.ChunkSize), n);
		pbIndex += n;

		n = sizeof(DWORD);
		CopyMemory(pbIndex, pIndex->time.Magic, n);
		pbIndex += n;
		n = 8;
		CopyMemory(pbIndex, &(pIndex->time.ChunkSize), n);
		pbIndex += n;
		n = sizeof(pIndex->time.FileTime);
		CopyMemory(pbIndex, &(pIndex->time.FileTime), n);
		pbIndex += n;

		n = sizeof(DWORD);
		CopyMemory(pbIndex, pIndex->adlr.Magic, n);
		pbIndex += n;
		n = 8;
		CopyMemory(pbIndex, &(pIndex->adlr.ChunkSize), n);
		pbIndex += n;
		n = 4;
		CopyMemory(pbIndex, &(pIndex->adlr.Hash), n);
		pbIndex += n;

		n = 4;
		CopyMemory(pbIndex, &(pIndex->segm.Magic), n);
		pbIndex += n;
		n = 8;
		CopyMemory(pbIndex, &(pIndex->segm.ChunkSize), n);
		pbIndex += n;
		n = 4;
		CopyMemory(pbIndex, &(pIndex->segm.segm[0].bZlib), n);
		pbIndex += n;
		n = 8;
		CopyMemory(pbIndex, &(pIndex->segm.segm[0].Offset), n);
		pbIndex += n;
		CopyMemory(pbIndex, &(pIndex->segm.segm[0].OriginalSize), n);
		pbIndex += n;
		CopyMemory(pbIndex, &(pIndex->segm.segm[0].ArchiveSize), n);
		pbIndex += n;

		n = 4;
		CopyMemory(pbIndex, &(pIndex->info.Magic), n);
		pbIndex += n;
		n = 8;
		CopyMemory(pbIndex, &(pIndex->info.ChunkSize), n);
		pbIndex += n;
		n = 4;
		CopyMemory(pbIndex, &(pIndex->info.EncryptedFlag), n);
		pbIndex += n;
		n = 8;
		CopyMemory(pbIndex, &(pIndex->info.OriginalSize), n);
		pbIndex += n;
		CopyMemory(pbIndex, &(pIndex->info.ArchiveSize), n);
		pbIndex += n;
		n = 2;
		CopyMemory(pbIndex, &(pIndex->info.FileNameLength), n);
		pbIndex += n;

		if (InfoNameZeroEnd)
		{
			n = (pIndex->info.FileName.length() + 1) * 2;
			CopyMemory(pbIndex, pIndex->info.FileName.c_str(), n);
		}
		else
		{
			n = (pIndex->info.FileName.length()) * 2;
			CopyMemory(pbIndex, pIndex->info.FileName.c_str(), n);
		}
		pbIndex += n;
	}

	IndexHeader.bZlib = 1;
	IndexHeader.OriginalSize.QuadPart = Size.LowPart;
	IndexHeader.ArchiveSize.LowPart = BufferSize;
	BufferSize = Size.LowPart;
	compress2((PBYTE)lpBuffer, &IndexHeader.ArchiveSize.LowPart, (PBYTE)lpCompressBuffer, BufferSize, Z_BEST_COMPRESSION);
	IndexHeader.ArchiveSize.HighPart = 0;

	FileXP3.Write(&IndexHeader, sizeof(IndexHeader), &BytesTransfered);
	FileXP3.Write(lpBuffer, IndexHeader.ArchiveSize.LowPart, &BytesTransfered);
	Offset.QuadPart = 0;
	FileXP3.Seek(Offset, FILE_BEGIN);
	FileXP3.Write(&XP3Header, sizeof(XP3Header), &BytesTransfered);

	FileXP3.Close();

	FreeMemoryP(lpBuffer);
	FreeMemoryP(lpCompressBuffer);
	FreeMemoryP(pXP3Index);
	return STATUS_SUCCESS;
}

NTSTATUS NTAPI KrkrPacker::IterFiles(LPCWSTR lpPath)
{
	NTSTATUS         Status;
	HANDLE           hFind;
	GlobalData*      Handle;
	WCHAR            FilePath[MAX_PATH];
	WIN32_FIND_DATAW FindFileData;

	Handle = GlobalData::GetGlobalData();

	RtlZeroMemory(&FindFileData, sizeof(WIN32_FIND_DATAW));
	RtlZeroMemory(FilePath, countof(FilePath) * sizeof(WCHAR));

	FormatStringW(FilePath, L"%s%s", lpPath, L"\\*.*");

	hFind = FindFirstFileW(FilePath, &FindFileData);
	if (hFind == INVALID_HANDLE_VALUE)
		return STATUS_NO_SUCH_FILE;

	Status = STATUS_SUCCESS;

	do
	{
		if (!StrCompareW(FindFileData.cFileName, L".") || !StrCompareW(FindFileData.cFileName, L".."))
			continue;
		else if (FindFileData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
			continue;
		else
		{
			if (Handle->DebugOn)
				PrintConsoleW(L"Found file : %s\n", FindFileData.cFileName);

			FileList.push_back(FindFileData.cFileName);
		}
	} while (FindNextFileW(hFind, &FindFileData));

	return Status;
}

wstring GetPackageNamePacker(wstring& FileName)
{
	wstring Temp;
	wstring::size_type Pos = FileName.find_last_of(L'\\');

	if (Pos != wstring::npos)
		Temp = FileName.substr(Pos + 1, wstring::npos);
	else
		Temp = FileName;

	Pos = Temp.find_last_of(L'/');
	if (Pos != wstring::npos)
		Temp = Temp.substr(Pos + 1, wstring::npos);
	
	return Temp;
}

void FormatPathNormal(wstring& package, ttstr& out)
{
	out.Clear();
	out = L"file://./";
	for (unsigned int iPos = 0; iPos < package.length(); iPos++)
	{
		if (package[iPos] == L':')
			continue;
		else if (package[iPos] == L'\\')
			out += L'/';
		else
			out += package[iPos];
	}
	out += L'>';
}


void FormatPathM2(wstring& package, ttstr& out)
{
	out.Clear();
	out = L"archive://./";
	out += GetPackageNamePacker(package).c_str();
	out += L"/";
}


NTSTATUS WINAPI KrkrPacker::DoM2DummyPackFirst(LPCWSTR lpBasePack)
{
	NTSTATUS                Status;
	GlobalData*             Handle;
	BOOL                    Result;
	NtFileDisk              File, FileXP3;
	PBYTE                   pbIndex;
	ULONG                   BufferSize, CompressedSize;
	PVOID                   lpBuffer, lpCompressBuffer;
	LARGE_INTEGER           Size, Offset, BytesTransfered;
	SMyXP3IndexM2            *pXP3Index, *pIndex;
	KRKR2_XP3_DATA_HEADER   IndexHeader;
	BYTE                    FirstMagic[11] = { 0x58, 0x50, 0x33, 0x0D, 0x0A, 0x20, 0x0A, 0x1A, 0x8B, 0x67, 0x01 };
	KRKR2_XP3_HEADER        XP3Header(FirstMagic, (ULONG64)0);
	wstring                 WStrBasePath(lpBasePack);
	WCHAR                   CurTempFileName[MAX_PATH];
	ULONG64                 RandNum;

	Handle = GlobalData::GetGlobalData();

	RandNum = genrand64_int64();
	RtlZeroMemory(CurTempFileName, countof(CurTempFileName) * 2);
	FormatStringW(CurTempFileName, L"KrkrzTempWorker_%08x%08x.xp3", HIDWORD(RandNum), LODWORD(RandNum));
	Handle->CurrentTempFileName = CurTempFileName;

	Status = FileXP3.Create(Handle->CurrentTempFileName.c_str());
	if (NT_FAILED(Status))
	{
		MessageBoxW(NULL, L"Couldn't create a handle for temporary output file.", L"KrkrExtract", MB_OK);
		return Status;
	}

	BufferSize = 0x10000;
	CompressedSize = BufferSize;
	lpBuffer = AllocateMemoryP(BufferSize);
	lpCompressBuffer = AllocateMemoryP(CompressedSize);
	pXP3Index = (SMyXP3IndexM2 *)AllocateMemoryP(sizeof(*pXP3Index) * FileList.size());
	pIndex = pXP3Index;

	if (!lpBuffer || !lpCompressBuffer || !pXP3Index)
	{
		MessageBoxW(Handle->MainWindow, L"Insufficient memory to make package!!", L"KrkrExtract", MB_OK | MB_ICONERROR);
		FileXP3.Close();
		
		if (lpBuffer)         FreeMemoryP(lpBuffer);
		if (lpCompressBuffer) FreeMemoryP(lpCompressBuffer);
		if (pXP3Index)        FreeMemoryP(pXP3Index);
		return STATUS_INSUFFICIENT_RESOURCES;
	}

	FileXP3.Write(&XP3Header, sizeof(XP3Header), &BytesTransfered);

	Offset.QuadPart = BytesTransfered.QuadPart;

	if (FileList.size() == 0)
	{
		MessageBoxW(Handle->MainWindow, L"No File to be packed", L"KrkrExtract", MB_OK);
		FileXP3.Close();
		FreeMemoryP(lpBuffer);
		FreeMemoryP(lpCompressBuffer);
		FreeMemoryP(pXP3Index);
		return STATUS_UNSUCCESSFUL;
	}

	for (ULONG i = 0; i < FileList.size(); ++pIndex, i++)
	{
		if (Handle->MainWindow)
		{
			WCHAR OutInfo[MAX_PATH];
			RtlZeroMemory(OutInfo, sizeof(OutInfo));
			FormatStringW(OutInfo, PackingFormatString, i + 1, FileList.size());
			SetWindowTextW(Handle->MainWindow, OutInfo);
		}

		ZeroMemory(pIndex, sizeof(*pIndex));
		*(PDWORD)(pIndex->yuzu.Magic) = Handle->M2ChunkMagic;
		*(PDWORD)(pIndex->file.Magic) = CHUNK_MAGIC_FILE;
		*(PDWORD)(pIndex->info.Magic) = CHUNK_MAGIC_INFO;
		*(PDWORD)(pIndex->time.Magic) = CHUNK_MAGIC_TIME;
		*(PDWORD)(pIndex->segm.Magic) = CHUNK_MAGIC_SEGM;
		*(PDWORD)(pIndex->adlr.Magic) = CHUNK_MAGIC_ADLR;
		pIndex->segm.ChunkSize.QuadPart = (ULONG64)0x1C; //sizeof(pIndex->segm.segm);
		//pIndex->adlr.ChunkSize.QuadPart = sizeof(pIndex->adlr) - sizeof(pIndex->adlr.Magic) - sizeof(pIndex->adlr.ChunkSize);
		pIndex->adlr.ChunkSize.QuadPart = (ULONG64)0x04;
		pIndex->info.ChunkSize.QuadPart = (ULONG64)0x58;

		if (M2NameZeroEnd)
			pIndex->file.ChunkSize.QuadPart = (ULONG64)0xB0;
		else
			pIndex->file.ChunkSize.QuadPart = (ULONG64)0xB0 - 2;

		pIndex->time.ChunkSize.QuadPart = (ULONG64)0x08;

		wstring FullName = wstring(lpBasePack) + L"\\";
		FullName += FileList[i];

		Status = File.Open(FullName.c_str());

		if (NT_FAILED(Status))
		{
			wstring InfoW(L"Dummy write :\n[Failed to open]\n");
			InfoW += FullName;
			MessageBoxW(Handle->MainWindow, InfoW.c_str(), L"KrkrExtract", MB_OK);
			FileXP3.Close();
			FreeMemoryP(lpBuffer);
			FreeMemoryP(lpCompressBuffer);
			FreeMemoryP(pXP3Index);
			return Status;
		}

		File.GetSize(&Size);
		if (Size.LowPart > BufferSize)
		{
			BufferSize = Size.LowPart;
			lpBuffer = ReAllocateMemoryP(lpBuffer, BufferSize);
		}

		File.Read(lpBuffer, Size.LowPart, &BytesTransfered);

		if (BytesTransfered.LowPart != Size.LowPart)
		{
			wstring InfoW(L"Dummy write :\n[Failed to read]\n");
			InfoW += FullName;
			MessageBoxW(Handle->MainWindow, InfoW.c_str(), L"KrkrExtract", MB_OK);
			File.Close();
			FileXP3.Close();
			FreeMemoryP(lpBuffer);
			FreeMemoryP(lpCompressBuffer);
			FreeMemoryP(pXP3Index);
			return STATUS_IO_DEVICE_ERROR;
		}

		pIndex->segm.segm->Offset = Offset;

		wstring DummyName, DummyLowerName, HashName;
		
		DummyName = FileList[i] + L".dummy";
		DummyLowerName = ToLowerString(DummyName.c_str());

		GenMD5Code(DummyLowerName.c_str(), HashName);
		pIndex->info.FileName = HashName;
		pIndex->info.FileNameLength = HashName.length();

		pIndex->yuzu.Len = DummyName.length();
		pIndex->yuzu.Name = DummyName;

		if (M2NameZeroEnd)
			pIndex->yuzu.ChunkSize.QuadPart = sizeof(DWORD)+sizeof(USHORT)+(pIndex->yuzu.Name.length() + 1) * 2;
		else
			pIndex->yuzu.ChunkSize.QuadPart = sizeof(DWORD)+sizeof(USHORT)+(pIndex->yuzu.Name.length()) * 2;

		pIndex->adlr.Hash = M2Hash;
		pIndex->yuzu.Hash = pIndex->adlr.Hash;

		pIndex->segm.segm->OriginalSize.LowPart = BytesTransfered.LowPart;
		pIndex->info.OriginalSize.LowPart = BytesTransfered.LowPart;

		FILETIME Time1, Time2;
		GetFileTime(File.GetHandle(), &(pIndex->time.FileTime), &Time1, &Time2);
		File.Close();
		LARGE_INTEGER EncryptOffset;

		EncryptOffset.QuadPart = 0;

		if (XP3EncryptionFlag)
			pIndex->info.EncryptedFlag = 0x80000000;
		else
			pIndex->info.EncryptedFlag = 0;

		PackChunkList.push_back(*pIndex);

		pIndex->file.ChunkSize.QuadPart = (ULONG64)sizeof(pIndex->time.ChunkSize) + MagicLength + sizeof(pIndex->time.FileTime) +
			sizeof(pIndex->info.ArchiveSize) * 2 + sizeof(pIndex->info.EncryptedFlag) + sizeof(USHORT) + sizeof(pIndex->info.ChunkSize) + MagicLength + (pIndex->info.FileName.length() + 1) * 2 +
			sizeof(pIndex->segm.ChunkSize) + MagicLength + sizeof(pIndex->segm.segm[0].ArchiveSize) * 3 + sizeof(BOOL) +
			sizeof(pIndex->adlr.ChunkSize) + MagicLength + sizeof(pIndex->adlr.Hash);

		if ((M2NameZeroEnd && (!InfoNameZeroEnd)) || ((!M2NameZeroEnd) && InfoNameZeroEnd))
			pIndex->file.ChunkSize.QuadPart -= 2;
		else if (!M2NameZeroEnd && !InfoNameZeroEnd)
			pIndex->file.ChunkSize.QuadPart -= 4;
		
		pIndex->segm.segm->bZlib = 0;


		pIndex->segm.segm->ArchiveSize.LowPart = BytesTransfered.LowPart;
		pIndex->info.ArchiveSize.LowPart = BytesTransfered.LowPart;
		Offset.QuadPart += BytesTransfered.QuadPart;

		FileXP3.Write(lpBuffer, BytesTransfered.LowPart, &BytesTransfered);
	}

	XP3Header.IndexOffset = Offset;

	// generate index, calculate index size first
	Size.LowPart = 0;
	pIndex = pXP3Index;

	for (ULONG i = 0; i < FileList.size(); ++i, ++pIndex)
	{
		Size.LowPart += sizeof(pIndex->yuzu.Hash) * 3 + sizeof(USHORT) + (pIndex->yuzu.Name.length() + 1) * 2 + MagicLength +
			sizeof(pIndex->time.ChunkSize) + MagicLength + sizeof(pIndex->time.FileTime) +
			sizeof(pIndex->file.ChunkSize) + MagicLength +
			sizeof(pIndex->info.ArchiveSize) * 2 + sizeof(pIndex->info.EncryptedFlag) + sizeof(USHORT) + sizeof(pIndex->info.ChunkSize) + MagicLength + (pIndex->info.FileName.length() + 1) * 2 +
			sizeof(pIndex->segm.ChunkSize) + MagicLength + sizeof(pIndex->segm.segm[0].ArchiveSize) * 3 + sizeof(BOOL) +
			sizeof(pIndex->adlr.ChunkSize) + MagicLength + sizeof(pIndex->adlr.Hash);

		if ((M2NameZeroEnd && (!InfoNameZeroEnd)) || ((!M2NameZeroEnd) && InfoNameZeroEnd))
			Size.LowPart -= 2;
		else if (!M2NameZeroEnd && !InfoNameZeroEnd)
			Size.LowPart -= 4;
	}

	if (Size.LowPart > CompressedSize)
	{
		CompressedSize = Size.LowPart;
		lpCompressBuffer = ReAllocateMemoryP(lpCompressBuffer, CompressedSize);
	}
	if (Size.LowPart * 2 > BufferSize)
	{
		BufferSize = Size.LowPart * 2;
		lpBuffer = ReAllocateMemoryP(lpBuffer, BufferSize);
	}

	// generate index to lpCompressBuffer
	pIndex = pXP3Index;
	pbIndex = (PBYTE)lpCompressBuffer;
	for (ULONG i = 0; i < FileList.size(); ++pIndex, i++)
	{
		ULONG n = sizeof(DWORD);
		CopyMemory(pbIndex, &(pIndex->yuzu.Magic), n);
		pbIndex += n;
		n = 8;
		CopyMemory(pbIndex, &(pIndex->yuzu.ChunkSize), n);
		pbIndex += n;
		n = 4;
		CopyMemory(pbIndex, &(pIndex->yuzu.Hash), n);
		pbIndex += n;
		n = sizeof(USHORT);
		CopyMemory(pbIndex, &(pIndex->yuzu.Len), n);
		pbIndex += n;

		if (M2NameZeroEnd)
		{
			n = (pIndex->yuzu.Name.length() + 1) * 2;
			CopyMemory(pbIndex, (pIndex->yuzu.Name.c_str()), n);
		}
		else
		{
			n = (pIndex->yuzu.Name.length()) * 2;
			CopyMemory(pbIndex, (pIndex->yuzu.Name.c_str()), n);
		}
		pbIndex += n;

		n = sizeof(DWORD);
		CopyMemory(pbIndex, pIndex->file.Magic, n);
		pbIndex += n;
		n = 8;
		CopyMemory(pbIndex, &(pIndex->file.ChunkSize), n);
		pbIndex += n;

		n = sizeof(DWORD);
		CopyMemory(pbIndex, pIndex->time.Magic, n);
		pbIndex += n;
		n = 8;
		CopyMemory(pbIndex, &(pIndex->time.ChunkSize), n);
		pbIndex += n;
		n = sizeof(pIndex->time.FileTime);
		CopyMemory(pbIndex, &(pIndex->time.FileTime), n);
		pbIndex += n;

		n = sizeof(DWORD);
		CopyMemory(pbIndex, pIndex->adlr.Magic, n);
		pbIndex += n;
		n = 8;
		CopyMemory(pbIndex, &(pIndex->adlr.ChunkSize), n);
		pbIndex += n;
		n = 4;
		CopyMemory(pbIndex, &(pIndex->adlr.Hash), n);
		pbIndex += n;

		n = 4;
		CopyMemory(pbIndex, &(pIndex->segm.Magic), n);
		pbIndex += n;
		n = 8;
		CopyMemory(pbIndex, &(pIndex->segm.ChunkSize), n);
		pbIndex += n;
		n = 4;
		CopyMemory(pbIndex, &(pIndex->segm.segm[0].bZlib), n);
		pbIndex += n;
		n = 8;
		CopyMemory(pbIndex, &(pIndex->segm.segm[0].Offset), n);
		pbIndex += n;
		CopyMemory(pbIndex, &(pIndex->segm.segm[0].OriginalSize), n);
		pbIndex += n;
		CopyMemory(pbIndex, &(pIndex->segm.segm[0].ArchiveSize), n);
		pbIndex += n;

		n = 4;
		CopyMemory(pbIndex, &(pIndex->info.Magic), n);
		pbIndex += n;
		n = 8;
		CopyMemory(pbIndex, &(pIndex->info.ChunkSize), n);
		pbIndex += n;
		n = 4;
		CopyMemory(pbIndex, &(pIndex->info.EncryptedFlag), n);
		pbIndex += n;
		n = 8;
		CopyMemory(pbIndex, &(pIndex->info.OriginalSize), n);
		pbIndex += n;
		CopyMemory(pbIndex, &(pIndex->info.ArchiveSize), n);
		pbIndex += n;
		n = 2;
		CopyMemory(pbIndex, &(pIndex->info.FileNameLength), n);
		pbIndex += n;

		if (InfoNameZeroEnd)
		{
			n = (pIndex->info.FileName.length() + 1) * 2;
			CopyMemory(pbIndex, pIndex->info.FileName.c_str(), n);
		}
		else
		{
			n = (pIndex->info.FileName.length()) * 2;
			CopyMemory(pbIndex, pIndex->info.FileName.c_str(), n);
		}
		pbIndex += n;
	}

	IndexHeader.bZlib = 1;
	IndexHeader.OriginalSize.QuadPart = Size.LowPart;
	IndexHeader.ArchiveSize.LowPart = BufferSize;
	BufferSize = Size.LowPart;
	compress2((PBYTE)lpBuffer, &IndexHeader.ArchiveSize.LowPart, (PBYTE)lpCompressBuffer, BufferSize, Z_BEST_COMPRESSION);
	IndexHeader.ArchiveSize.HighPart = 0;

	FileXP3.Write(&IndexHeader, sizeof(IndexHeader), &BytesTransfered);
	FileXP3.Write(lpBuffer, IndexHeader.ArchiveSize.LowPart, &BytesTransfered);
	Offset.QuadPart = 0;
	FileXP3.Seek(Offset, FILE_BEGIN);
	FileXP3.Write( &XP3Header, sizeof(XP3Header), &BytesTransfered);

	FileXP3.Close();

	FreeMemoryP(lpBuffer);
	FreeMemoryP(lpCompressBuffer);
	FreeMemoryP(pXP3Index);
	PackChunkList.clear();
	return STATUS_SUCCESS;
}



NTSTATUS WINAPI KrkrPacker::DoM2DummyPackFirst_Version2(LPCWSTR lpBasePack)
{
	NTSTATUS                Status;
	GlobalData*             Handle;
	BOOL                    Result;
	NtFileDisk              File, FileXP3;
	PBYTE                   pbIndex;
	ULONG                   BufferSize, CompressedSize;
	PVOID                   lpBuffer, lpCompressBuffer;
	LARGE_INTEGER           Size, Offset, BytesTransfered;
	SMyXP3IndexM2            *pXP3Index, *pIndex;
	KRKR2_XP3_DATA_HEADER   IndexHeader;
	BYTE                    FirstMagic[11] = { 0x58, 0x50, 0x33, 0x0D, 0x0A, 0x20, 0x0A, 0x1A, 0x8B, 0x67, 0x01 };
	KRKR2_XP3_HEADER        XP3Header(FirstMagic, (ULONG64)0);
	wstring                 WStrBasePath(lpBasePack);
	WCHAR                   CurTempFileName[MAX_PATH];
	ULONG64                 RandNum;


	Handle = GlobalData::GetGlobalData();

	RandNum = genrand64_int64();
	RtlZeroMemory(CurTempFileName, countof(CurTempFileName) * 2);
	FormatStringW(CurTempFileName, L"KrkrzTempWorker_%08x%08x.xp3", HIDWORD(RandNum), LODWORD(RandNum));
	Handle->CurrentTempFileName = CurTempFileName;

	Status = FileXP3.Create(Handle->CurrentTempFileName.c_str());

	if (NT_FAILED(Status))
	{
		MessageBoxW(NULL, L"Couldn't create a handle for temporary output file.", L"KrkrExtract", MB_OK);
		return Status;
	}

	BufferSize = 0x10000;
	CompressedSize = BufferSize;
	lpBuffer = AllocateMemoryP(BufferSize);
	lpCompressBuffer = AllocateMemoryP(CompressedSize);
	pXP3Index = (SMyXP3IndexM2 *)AllocateMemoryP(sizeof(*pXP3Index) * FileList.size());
	pIndex = pXP3Index;

	if (!lpBuffer || !lpCompressBuffer || !pXP3Index)
	{
		MessageBoxW(Handle->MainWindow, L"Insufficient memory to make package!!", L"KrkrExtract", MB_OK | MB_ICONERROR);
		FileXP3.Close();
		
		if (lpBuffer)         FreeMemoryP(lpBuffer);
		if (lpCompressBuffer) FreeMemoryP(lpCompressBuffer);
		if (pXP3Index)        FreeMemoryP(pXP3Index);
		return STATUS_INSUFFICIENT_RESOURCES;
	}

	FileXP3.Write(&XP3Header, sizeof(XP3Header), &BytesTransfered);

	Offset.QuadPart = BytesTransfered.QuadPart;

	if (FileList.size() == 0)
	{
		MessageBoxW(NULL, L"No File to be packed", L"KrkrExtract", MB_OK);
		FileXP3.Close();
		FreeMemoryP(lpBuffer);
		FreeMemoryP(lpCompressBuffer);
		FreeMemoryP(pXP3Index);
		return STATUS_UNSUCCESSFUL;
	}

	for (ULONG i = 0; i < FileList.size(); ++pIndex, i++)
	{
		if (Handle->MainWindow)
		{
			WCHAR OutInfo[MAX_PATH];
			FormatStringW(OutInfo, PackingFormatString, i + 1, FileList.size());
			SetWindowTextW(Handle->MainWindow, OutInfo);
		}

		ZeroMemory(pIndex, sizeof(*pIndex));

		*(PDWORD)(pIndex->file.Magic) = CHUNK_MAGIC_FILE;
		*(PDWORD)(pIndex->info.Magic) = CHUNK_MAGIC_INFO;
		*(PDWORD)(pIndex->time.Magic) = CHUNK_MAGIC_TIME;
		*(PDWORD)(pIndex->segm.Magic) = CHUNK_MAGIC_SEGM;
		*(PDWORD)(pIndex->adlr.Magic) = CHUNK_MAGIC_ADLR;
		*(PDWORD)(pIndex->yuzu.Magic) = Handle->M2ChunkMagic;

		pIndex->segm.ChunkSize.QuadPart = (ULONG64)0x1C; //sizeof(pIndex->segm.segm);
		//pIndex->adlr.ChunkSize.QuadPart = sizeof(pIndex->adlr) - sizeof(pIndex->adlr.Magic) - sizeof(pIndex->adlr.ChunkSize);
		pIndex->adlr.ChunkSize.QuadPart = (ULONG64)0x04;
		pIndex->info.ChunkSize.QuadPart = (ULONG64)0x58;

		if (M2NameZeroEnd)
			pIndex->file.ChunkSize.QuadPart = (ULONG64)0xB0;
		else
			pIndex->file.ChunkSize.QuadPart = (ULONG64)0xB0 - 2;
		
		pIndex->time.ChunkSize.QuadPart = (ULONG64)0x08;

		wstring FullName = wstring(lpBasePack) + L"\\";
		FullName += FileList[i];
		Status = File.Open(FullName.c_str());

		if (NT_FAILED(Status))
		{
			wstring InfoW(L"Dummy write :\n[Failed to open]\n");
			InfoW += FullName;
			MessageBoxW(Handle->MainWindow, InfoW.c_str(), L"KrkrExtract", MB_OK);
			FileXP3.Close();
			FreeMemoryP(lpBuffer);
			FreeMemoryP(lpCompressBuffer);
			FreeMemoryP(pXP3Index);
			return Status;
		}

		File.GetSize(&Size);
		if (Size.LowPart > BufferSize)
		{
			BufferSize = Size.LowPart;
			lpBuffer = ReAllocateMemoryP(lpBuffer, BufferSize);
		}

		File.Read(lpBuffer, Size.LowPart, &BytesTransfered);
		
		if (BytesTransfered.LowPart != Size.LowPart)
		{
			wstring InfoW(L"Dummy write :\n[Failed to read]\n");
			InfoW += FullName;
			MessageBoxW(Handle->MainWindow, InfoW.c_str(), L"KrkrExtract", MB_OK);
			File.Close();
			FileXP3.Close();
			FreeMemoryP(lpBuffer);
			FreeMemoryP(lpCompressBuffer);
			FreeMemoryP(pXP3Index);
			return STATUS_IO_DEVICE_ERROR;
		}

		pIndex->segm.segm->Offset = Offset;

		wstring DummyName, DummyLowerName, HashName;

		DummyName = FileList[i] + L".dummy";
		DummyLowerName = ToLowerString(DummyName.c_str());

		GenMD5Code(DummyLowerName.c_str(), HashName);
		pIndex->info.FileName = HashName;
		pIndex->info.FileNameLength = HashName.length();

		pIndex->yuzu.Len = DummyName.length();
		pIndex->yuzu.Name = DummyName;

		if (M2NameZeroEnd)
			pIndex->yuzu.ChunkSize.QuadPart = sizeof(DWORD) + sizeof(USHORT) + (pIndex->yuzu.Name.length() + 1) * 2;
		else
			pIndex->yuzu.ChunkSize.QuadPart = sizeof(DWORD) + sizeof(USHORT) + (pIndex->yuzu.Name.length()) * 2;

		//adler32(1/*adler32(0, 0, 0)*/, (Bytef *)lpBuffer, BytesTransfered);
		pIndex->adlr.Hash = M2Hash;
		pIndex->yuzu.Hash = pIndex->adlr.Hash;

		pIndex->segm.segm->OriginalSize.LowPart = BytesTransfered.LowPart;
		pIndex->info.OriginalSize.LowPart = BytesTransfered.LowPart;

		FILETIME Time1, Time2;
		GetFileTime(File.GetHandle(), &(pIndex->time.FileTime), &Time1, &Time2);
		File.Close();
		LARGE_INTEGER EncryptOffset;

		EncryptOffset.QuadPart = 0;

		if (XP3EncryptionFlag)
			pIndex->info.EncryptedFlag = 0x80000000;
		else
			pIndex->info.EncryptedFlag = 0;

		PackChunkList.push_back(*pIndex);

		if (M2NameZeroEnd && InfoNameZeroEnd)
		{
			pIndex->file.ChunkSize.QuadPart = (ULONG64)sizeof(pIndex->time.ChunkSize) + MagicLength + sizeof(pIndex->time.FileTime) +
				sizeof(pIndex->info.ArchiveSize) * 2 + sizeof(pIndex->info.EncryptedFlag) + sizeof(USHORT) + sizeof(pIndex->info.ChunkSize) + MagicLength + (pIndex->info.FileName.length() + 1) * 2 +
				sizeof(pIndex->segm.ChunkSize) + MagicLength + sizeof(pIndex->segm.segm[0].ArchiveSize) * 3 + sizeof(BOOL) +
				sizeof(pIndex->adlr.ChunkSize) + MagicLength + sizeof(pIndex->adlr.Hash);
		}
		else if ((M2NameZeroEnd && (!InfoNameZeroEnd)) || ((!M2NameZeroEnd) && InfoNameZeroEnd))
		{
			pIndex->file.ChunkSize.QuadPart = (ULONG64)sizeof(pIndex->time.ChunkSize) + MagicLength + sizeof(pIndex->time.FileTime) +
				sizeof(pIndex->info.ArchiveSize) * 2 + sizeof(pIndex->info.EncryptedFlag) + sizeof(USHORT) + sizeof(pIndex->info.ChunkSize) + MagicLength + (pIndex->info.FileName.length() + 1) * 2 +
				sizeof(pIndex->segm.ChunkSize) + MagicLength + sizeof(pIndex->segm.segm[0].ArchiveSize) * 3 + sizeof(BOOL) +
				sizeof(pIndex->adlr.ChunkSize) + MagicLength + sizeof(pIndex->adlr.Hash) - 2;
		}
		else
		{
			pIndex->file.ChunkSize.QuadPart = (ULONG64)sizeof(pIndex->time.ChunkSize) + MagicLength + sizeof(pIndex->time.FileTime) +
				sizeof(pIndex->info.ArchiveSize) * 2 + sizeof(pIndex->info.EncryptedFlag) + sizeof(USHORT) + sizeof(pIndex->info.ChunkSize) + MagicLength + (pIndex->info.FileName.length() + 1) * 2 +
				sizeof(pIndex->segm.ChunkSize) + MagicLength + sizeof(pIndex->segm.segm[0].ArchiveSize) * 3 + sizeof(BOOL) +
				sizeof(pIndex->adlr.ChunkSize) + MagicLength + sizeof(pIndex->adlr.Hash) - 4;
		}

		pIndex->segm.segm->bZlib = 0;

		pIndex->segm.segm->ArchiveSize.LowPart = BytesTransfered.LowPart;
		pIndex->info.ArchiveSize.LowPart = BytesTransfered.LowPart;
		Offset.QuadPart += BytesTransfered.QuadPart;

		FileXP3.Write(lpBuffer, BytesTransfered.LowPart, &BytesTransfered);
	}

	XP3Header.IndexOffset = Offset;

	// generate index, calculate index size first
	Size.LowPart = 0;
	pIndex = pXP3Index;

	for (ULONG i = 0; i < FileList.size(); ++i, ++pIndex)
	{
		Size.LowPart += sizeof(pIndex->yuzu.Hash) * 3 + sizeof(USHORT) + (pIndex->yuzu.Name.length() + 1) * 2 + MagicLength +
			sizeof(pIndex->time.ChunkSize) + MagicLength + sizeof(pIndex->time.FileTime) +
			sizeof(pIndex->file.ChunkSize) + MagicLength +
			sizeof(pIndex->info.ArchiveSize) * 2 + sizeof(pIndex->info.EncryptedFlag) + sizeof(USHORT) + sizeof(pIndex->info.ChunkSize) + MagicLength + (pIndex->info.FileName.length() + 1) * 2 +
			sizeof(pIndex->segm.ChunkSize) + MagicLength + sizeof(pIndex->segm.segm[0].ArchiveSize) * 3 + sizeof(BOOL) +
			sizeof(pIndex->adlr.ChunkSize) + MagicLength + sizeof(pIndex->adlr.Hash);

		if ((M2NameZeroEnd && (!InfoNameZeroEnd)) || ((!M2NameZeroEnd) && InfoNameZeroEnd))
			Size.LowPart -= 2;
		else
			Size.LowPart -= 4;
	}

	if (Size.LowPart > CompressedSize)
	{
		CompressedSize = Size.LowPart;
		lpCompressBuffer = ReAllocateMemoryP(lpCompressBuffer, CompressedSize);
	}
	if (Size.LowPart * 2 > BufferSize)
	{
		BufferSize = Size.LowPart * 2;
		lpBuffer = ReAllocateMemoryP(lpBuffer, BufferSize);
	}

	// generate index to lpCompressBuffer
	pIndex = pXP3Index;
	pbIndex = (PBYTE)lpCompressBuffer;
	for (ULONG i = 0; i < FileList.size(); ++pIndex, i++)
	{
		ULONG n = sizeof(DWORD);
		CopyMemory(pbIndex, &(pIndex->yuzu.Magic), n);
		pbIndex += n;
		n = 8;
		CopyMemory(pbIndex, &(pIndex->yuzu.ChunkSize), n);
		pbIndex += n;
		n = 4;
		CopyMemory(pbIndex, &(pIndex->yuzu.Hash), n);
		pbIndex += n;
		n = sizeof(USHORT);
		CopyMemory(pbIndex, &(pIndex->yuzu.Len), n);
		pbIndex += n;

		if (M2NameZeroEnd)
		{
			n = (pIndex->yuzu.Name.length() + 1) * 2;
			CopyMemory(pbIndex, (pIndex->yuzu.Name.c_str()), n);
		}
		else
		{
			n = (pIndex->yuzu.Name.length()) * 2;
			CopyMemory(pbIndex, (pIndex->yuzu.Name.c_str()), n);
		}
		pbIndex += n;
	}

	pIndex = pXP3Index;
	for (ULONG i = 0; i < FileList.size(); ++pIndex, i++)
	{
		ULONG n = sizeof(DWORD);
		CopyMemory(pbIndex, pIndex->file.Magic, n);
		pbIndex += n;
		n = 8;
		CopyMemory(pbIndex, &(pIndex->file.ChunkSize), n);
		pbIndex += n;

		n = sizeof(DWORD);
		CopyMemory(pbIndex, pIndex->time.Magic, n);
		pbIndex += n;
		n = 8;
		CopyMemory(pbIndex, &(pIndex->time.ChunkSize), n);
		pbIndex += n;
		n = sizeof(pIndex->time.FileTime);
		CopyMemory(pbIndex, &(pIndex->time.FileTime), n);
		pbIndex += n;

		n = sizeof(DWORD);
		CopyMemory(pbIndex, pIndex->adlr.Magic, n);
		pbIndex += n;
		n = 8;
		CopyMemory(pbIndex, &(pIndex->adlr.ChunkSize), n);
		pbIndex += n;
		n = 4;
		CopyMemory(pbIndex, &(pIndex->adlr.Hash), n);
		pbIndex += n;

		n = 4;
		CopyMemory(pbIndex, &(pIndex->segm.Magic), n);
		pbIndex += n;
		n = 8;
		CopyMemory(pbIndex, &(pIndex->segm.ChunkSize), n);
		pbIndex += n;
		n = 4;
		CopyMemory(pbIndex, &(pIndex->segm.segm[0].bZlib), n);
		pbIndex += n;
		n = 8;
		CopyMemory(pbIndex, &(pIndex->segm.segm[0].Offset), n);
		pbIndex += n;
		CopyMemory(pbIndex, &(pIndex->segm.segm[0].OriginalSize), n);
		pbIndex += n;
		CopyMemory(pbIndex, &(pIndex->segm.segm[0].ArchiveSize), n);
		pbIndex += n;

		n = 4;
		CopyMemory(pbIndex, &(pIndex->info.Magic), n);
		pbIndex += n;
		n = 8;
		CopyMemory(pbIndex, &(pIndex->info.ChunkSize), n);
		pbIndex += n;
		n = 4;
		CopyMemory(pbIndex, &(pIndex->info.EncryptedFlag), n);
		pbIndex += n;
		n = 8;
		CopyMemory(pbIndex, &(pIndex->info.OriginalSize), n);
		pbIndex += n;
		CopyMemory(pbIndex, &(pIndex->info.ArchiveSize), n);
		pbIndex += n;
		n = 2;
		CopyMemory(pbIndex, &(pIndex->info.FileNameLength), n);
		pbIndex += n;

		if (InfoNameZeroEnd)
		{
			n = (pIndex->info.FileName.length() + 1) * 2;
			CopyMemory(pbIndex, pIndex->info.FileName.c_str(), n);
		}
		else
		{
			n = (pIndex->info.FileName.length()) * 2;
			CopyMemory(pbIndex, pIndex->info.FileName.c_str(), n);
		}
		pbIndex += n;
	}

	IndexHeader.bZlib = 1;
	IndexHeader.OriginalSize.QuadPart = Size.LowPart;
	IndexHeader.ArchiveSize.LowPart = BufferSize;
	BufferSize = Size.LowPart;
	compress2((PBYTE)lpBuffer, &IndexHeader.ArchiveSize.LowPart, (PBYTE)lpCompressBuffer, BufferSize, Z_BEST_COMPRESSION);
	IndexHeader.ArchiveSize.HighPart = 0;

	FileXP3.Write(&IndexHeader, sizeof(IndexHeader), &BytesTransfered);
	FileXP3.Write(lpBuffer, IndexHeader.ArchiveSize.LowPart, &BytesTransfered);
	Offset.QuadPart = 0;
	FileXP3.Seek(Offset, FILE_BEGIN);
	FileXP3.Write(&XP3Header, sizeof(XP3Header), &BytesTransfered);

	FileXP3.Close();

	FreeMemoryP(lpBuffer);
	FreeMemoryP(lpCompressBuffer);
	FreeMemoryP(pXP3Index);
	return STATUS_SUCCESS;
}


NTSTATUS NTAPI KrkrPacker::DoM2Pack(LPCWSTR lpBasePack, LPCWSTR GuessPackage, LPCWSTR OutName)
{
	NTSTATUS                Status;
	BOOL                    Result;
	GlobalData*             Handle;
	NtFileDisk              FileXP3;
	PBYTE                   pbIndex;
	ULONG                   BufferSize, CompressedSize;
	PVOID                   lpBuffer, lpCompressBuffer;
	LARGE_INTEGER           Size, Offset, BytesTransfered;
	SMyXP3IndexM2            *pXP3Index, *pIndex;
	KRKR2_XP3_DATA_HEADER   IndexHeader;
	BYTE                    FirstMagic[11] = { 0x58, 0x50, 0x33, 0x0D, 0x0A, 0x20, 0x0A, 0x1A, 0x8B, 0x67, 0x01 };
	KRKR2_XP3_HEADER        XP3Header(FirstMagic, (ULONG64)0);
	wstring                 WStrBasePath(lpBasePack);

	Handle = GlobalData::GetGlobalData();

	FileList.clear();
	IterFiles(lpBasePack);

	Status = DoM2DummyPackFirst(lpBasePack);
	if (NT_FAILED(Status))
		return Status;

	TVPExecuteScript(ttstr(L"Storages.addAutoPath(System.exePath + \"" + ttstr(Handle->CurrentTempFileName.c_str()) + L"\" + \">\");"));

	Status = FileXP3.Create(OutName);
	if (NT_FAILED(Status))
	{
		MessageBoxW(Handle->MainWindow, L"Couldn't create a handle for output xp3 file.", L"KrkrExtract", MB_OK);
		return Status;
	}

	BufferSize = 0x10000;
	CompressedSize = BufferSize;
	lpBuffer = AllocateMemoryP(BufferSize);
	lpCompressBuffer = AllocateMemoryP(CompressedSize);
	pXP3Index = (SMyXP3IndexM2 *)AllocateMemoryP(sizeof(*pXP3Index) * FileList.size());
	pIndex = pXP3Index;

	if (!lpBuffer || !lpCompressBuffer || !pXP3Index)
	{
		MessageBoxW(Handle->MainWindow, L"Insufficient memory to make package!!", L"KrkrExtract", MB_OK | MB_ICONERROR);
		FileXP3.Close();
		
		if (lpBuffer)         FreeMemoryP(lpBuffer);
		if (lpCompressBuffer) FreeMemoryP(lpCompressBuffer);
		if (pXP3Index)        FreeMemoryP(pXP3Index);
		return STATUS_INSUFFICIENT_RESOURCES;
	}

	FileXP3.Write(&XP3Header, sizeof(XP3Header), &BytesTransfered);

	Offset.QuadPart = BytesTransfered.QuadPart;
	for (ULONG i = 0; i <FileList.size(); ++pIndex, i++)
	{
		if (Handle->MainWindow)
		{
			WCHAR OutInfo[MAX_PATH];
			FormatStringW(OutInfo, PackingFormatString, i + 1, FileList.size());
			SetWindowTextW(Handle->MainWindow, OutInfo);
			Handle->SetProcess(Handle->MainWindow, (ULONG)(((float)(i + 1) / (float)FileList.size()) * 100.0));
		}

		ZeroMemory(pIndex, sizeof(*pIndex));
		*(PDWORD)(pIndex->file.Magic) = CHUNK_MAGIC_FILE;
		*(PDWORD)(pIndex->info.Magic) = CHUNK_MAGIC_INFO;
		*(PDWORD)(pIndex->time.Magic) = CHUNK_MAGIC_TIME;
		*(PDWORD)(pIndex->segm.Magic) = CHUNK_MAGIC_SEGM;
		*(PDWORD)(pIndex->adlr.Magic) = CHUNK_MAGIC_ADLR;
		*(PDWORD)(pIndex->yuzu.Magic) = Handle->M2ChunkMagic;
		pIndex->segm.ChunkSize.QuadPart = (ULONG64)0x1C; //sizeof(pIndex->segm.segm);
		pIndex->adlr.ChunkSize.QuadPart = (ULONG64)0x04;
		pIndex->info.ChunkSize.QuadPart = (ULONG64)0x58;

		if (M2NameZeroEnd)
			pIndex->file.ChunkSize.QuadPart = (ULONG64)0xB0;
		else
			pIndex->file.ChunkSize.QuadPart = (ULONG64)0xB0 - 2;

		pIndex->time.ChunkSize.QuadPart = (ULONG64)0x08;

		wstring DummyName = FileList[i] + L".dummy";

		ttstr FullName(L"archive://./" + ttstr(Handle->CurrentTempFileName.c_str()) + L"/");
		
		FullName += DummyName.c_str();
		

		IStream* st = TVPCreateIStream(FullName, TJS_BS_READ);
		if (st == NULL)
		{
			wstring InfoW(L"Couldn't open :\n");
			InfoW += FileList[i];
			MessageBoxW(Handle->MainWindow, InfoW.c_str(), L"KrkrExtract", MB_OK);
			FileXP3.Close();
			FreeMemoryP(lpBuffer);
			FreeMemoryP(lpCompressBuffer);
			FreeMemoryP(pXP3Index);
			return STATUS_UNSUCCESSFUL;
		}

		STATSTG t;
		st->Stat(&t, STATFLAG_DEFAULT);
		Size.QuadPart = t.cbSize.QuadPart;
		if (Size.LowPart > BufferSize)
		{
			BufferSize = Size.LowPart;
			lpBuffer = ReAllocateMemoryP(lpBuffer, BufferSize);
		}
		st->Read(lpBuffer, Size.LowPart, &BytesTransfered.LowPart);
		
		if (BytesTransfered.LowPart != Size.LowPart)
		{
			wstring InfoW(L"Couldn't read :\n");
			InfoW += FileList[i];
			MessageBoxW(Handle->MainWindow, InfoW.c_str(), L"KrkrExtract", MB_OK);
			FileXP3.Close();
			FreeMemoryP(lpBuffer);
			FreeMemoryP(lpCompressBuffer);
			FreeMemoryP(pXP3Index);
			return STATUS_UNSUCCESSFUL;
		}

		pIndex->segm.segm->Offset = Offset;

		wstring LowerName = ToLowerString(FileList[i].c_str());
		wstring HashName;

		GenMD5Code(LowerName.c_str(), HashName);
		pIndex->info.FileName = HashName;
		pIndex->info.FileNameLength = HashName.length();

		pIndex->yuzu.Len = FileList[i].length();
		pIndex->yuzu.Name = FileList[i];

		if (M2NameZeroEnd)
			pIndex->yuzu.ChunkSize.QuadPart = sizeof(DWORD)+sizeof(USHORT)+(pIndex->yuzu.Name.length() + 1) * 2;
		else
			pIndex->yuzu.ChunkSize.QuadPart = sizeof(DWORD)+sizeof(USHORT)+(pIndex->yuzu.Name.length()) * 2;

		pIndex->adlr.Hash = M2Hash;
		pIndex->yuzu.Hash = pIndex->adlr.Hash;

		pIndex->segm.segm->OriginalSize.LowPart = BytesTransfered.LowPart;
		pIndex->info.OriginalSize.LowPart = BytesTransfered.LowPart;

		LARGE_INTEGER EncryptOffset;

		EncryptOffset.QuadPart = 0;

		if (XP3EncryptionFlag)
			pIndex->info.EncryptedFlag = 0x80000000;
		else
			pIndex->info.EncryptedFlag = 0x0;
		

		pIndex->file.ChunkSize.QuadPart = (ULONG64)sizeof(pIndex->time.ChunkSize) + MagicLength + sizeof(pIndex->time.FileTime) +
			sizeof(pIndex->info.ArchiveSize) * 2 + sizeof(pIndex->info.EncryptedFlag) + sizeof(USHORT) + sizeof(pIndex->info.ChunkSize) + MagicLength + (pIndex->info.FileName.length() + 1) * 2 +
			sizeof(pIndex->segm.ChunkSize) + MagicLength + sizeof(pIndex->segm.segm[0].ArchiveSize) * 3 + sizeof(BOOL) +
			sizeof(pIndex->adlr.ChunkSize) + MagicLength + sizeof(pIndex->adlr.Hash);

		if ((M2NameZeroEnd && (!InfoNameZeroEnd)) || ((!M2NameZeroEnd) && InfoNameZeroEnd))
			pIndex->file.ChunkSize.QuadPart -= 2;
		else if (!M2NameZeroEnd && !InfoNameZeroEnd)
			pIndex->file.ChunkSize.QuadPart -= 4;
		
		pIndex->segm.segm->bZlib = 0;

		pIndex->segm.segm->ArchiveSize.LowPart = BytesTransfered.LowPart;
		pIndex->info.ArchiveSize.LowPart = BytesTransfered.LowPart;
		Offset.QuadPart += BytesTransfered.QuadPart;

		FileXP3.Write(lpBuffer, BytesTransfered.LowPart, &BytesTransfered);
	}

	XP3Header.IndexOffset = Offset;

	Size.LowPart = 0;
	pIndex = pXP3Index;

	for (ULONG i = 0; i < FileList.size(); ++i, ++pIndex)
	{
		Size.LowPart += sizeof(pIndex->yuzu.Hash) * 3 + sizeof(USHORT) + (pIndex->yuzu.Name.length() + 1) * 2 + MagicLength +
			sizeof(pIndex->time.ChunkSize) + MagicLength + sizeof(pIndex->time.FileTime) +
			sizeof(pIndex->file.ChunkSize) + MagicLength +
			sizeof(pIndex->info.ArchiveSize) * 2 + sizeof(pIndex->info.EncryptedFlag) + sizeof(USHORT) + sizeof(pIndex->info.ChunkSize) + MagicLength + (pIndex->info.FileName.length() + 1) * 2 +
			sizeof(pIndex->segm.ChunkSize) + MagicLength + sizeof(pIndex->segm.segm[0].ArchiveSize) * 3 + sizeof(BOOL) +
			sizeof(pIndex->adlr.ChunkSize) + MagicLength + sizeof(pIndex->adlr.Hash);

		if ((M2NameZeroEnd && (!InfoNameZeroEnd)) || ((!M2NameZeroEnd) && InfoNameZeroEnd))
			Size.LowPart -= 2;
		else if (!M2NameZeroEnd && !InfoNameZeroEnd)
			Size.LowPart -= 4;
	}

	if (Size.LowPart > CompressedSize)
	{
		CompressedSize = Size.LowPart;
		lpCompressBuffer = ReAllocateMemoryP(lpCompressBuffer, CompressedSize);
	}
	if (Size.LowPart * 2 > BufferSize)
	{
		BufferSize = Size.LowPart * 2;
		lpBuffer = ReAllocateMemoryP(lpBuffer, BufferSize);
	}

	// generate index to lpCompressBuffer
	pIndex = pXP3Index;
	pbIndex = (PBYTE)lpCompressBuffer;
	for (ULONG i = 0; i < FileList.size(); ++pIndex, i++)
	{
		ULONG n = sizeof(DWORD);
		CopyMemory(pbIndex, &(pIndex->yuzu.Magic), n);
		pbIndex += n;
		n = 8;
		CopyMemory(pbIndex, &(pIndex->yuzu.ChunkSize), n);
		pbIndex += n;
		n = 4;
		CopyMemory(pbIndex, &(pIndex->yuzu.Hash), n);
		pbIndex += n;
		n = sizeof(USHORT);
		CopyMemory(pbIndex, &(pIndex->yuzu.Len), n);
		pbIndex += n;

		if (M2NameZeroEnd)
		{
			n = (pIndex->yuzu.Name.length() + 1) * 2;
			CopyMemory(pbIndex, (pIndex->yuzu.Name.c_str()), n);
		}
		else
		{
			n = (pIndex->yuzu.Name.length()) * 2;
			CopyMemory(pbIndex, (pIndex->yuzu.Name.c_str()), n);
		}
		pbIndex += n;

		n = sizeof(DWORD);
		CopyMemory(pbIndex, pIndex->file.Magic, n);
		pbIndex += n;
		n = 8;
		CopyMemory(pbIndex, &(pIndex->file.ChunkSize), n);
		pbIndex += n;

		n = sizeof(DWORD);
		CopyMemory(pbIndex, pIndex->time.Magic, n);
		pbIndex += n;
		n = 8;
		CopyMemory(pbIndex, &(pIndex->time.ChunkSize), n);
		pbIndex += n;
		n = sizeof(pIndex->time.FileTime);
		CopyMemory(pbIndex, &(pIndex->time.FileTime), n);
		pbIndex += n;

		n = sizeof(DWORD);
		CopyMemory(pbIndex, pIndex->adlr.Magic, n);
		pbIndex += n;
		n = 8;
		CopyMemory(pbIndex, &(pIndex->adlr.ChunkSize), n);
		pbIndex += n;
		n = 4;
		CopyMemory(pbIndex, &(pIndex->adlr.Hash), n);
		pbIndex += n;

		n = 4;
		CopyMemory(pbIndex, &(pIndex->segm.Magic), n);
		pbIndex += n;
		n = 8;
		CopyMemory(pbIndex, &(pIndex->segm.ChunkSize), n);
		pbIndex += n;
		n = 4;
		CopyMemory(pbIndex, &(pIndex->segm.segm[0].bZlib), n);
		pbIndex += n;
		n = 8;
		CopyMemory(pbIndex, &(pIndex->segm.segm[0].Offset), n);
		pbIndex += n;
		CopyMemory(pbIndex, &(pIndex->segm.segm[0].OriginalSize), n);
		pbIndex += n;
		CopyMemory(pbIndex, &(pIndex->segm.segm[0].ArchiveSize), n);
		pbIndex += n;

		n = 4;
		CopyMemory(pbIndex, &(pIndex->info.Magic), n);
		pbIndex += n;
		n = 8;
		CopyMemory(pbIndex, &(pIndex->info.ChunkSize), n);
		pbIndex += n;
		n = 4;
		CopyMemory(pbIndex, &(pIndex->info.EncryptedFlag), n);
		pbIndex += n;
		n = 8;
		CopyMemory(pbIndex, &(pIndex->info.OriginalSize), n);
		pbIndex += n;
		CopyMemory(pbIndex, &(pIndex->info.ArchiveSize), n);
		pbIndex += n;
		n = 2;
		CopyMemory(pbIndex, &(pIndex->info.FileNameLength), n);
		pbIndex += n;

		if (InfoNameZeroEnd)
		{
			n = (pIndex->info.FileName.length() + 1) * 2;
			CopyMemory(pbIndex, pIndex->info.FileName.c_str(), n);
		}
		else
		{
			n = (pIndex->info.FileName.length()) * 2;
			CopyMemory(pbIndex, pIndex->info.FileName.c_str(), n);
		}
		pbIndex += n;
	}

	IndexHeader.bZlib = 1;
	IndexHeader.OriginalSize.QuadPart = Size.LowPart;
	IndexHeader.ArchiveSize.LowPart = BufferSize;
	BufferSize = Size.LowPart;
	compress2((PBYTE)lpBuffer, &IndexHeader.ArchiveSize.LowPart, (PBYTE)lpCompressBuffer, BufferSize, Z_BEST_COMPRESSION);
	IndexHeader.ArchiveSize.HighPart = 0;

	FileXP3.Write(&IndexHeader, sizeof(IndexHeader), &BytesTransfered);
	FileXP3.Write(lpBuffer, IndexHeader.ArchiveSize.LowPart, &BytesTransfered);
	Offset.QuadPart = 0;
	FileXP3.Seek(Offset, FILE_BEGIN);
	FileXP3.Write(&XP3Header, sizeof(XP3Header), &BytesTransfered);

	FileXP3.Close();

	FreeMemoryP(lpBuffer);
	FreeMemoryP(lpCompressBuffer);
	FreeMemoryP(pXP3Index);

	TVPExecuteScript(ttstr(L"Storages.removeAutoPath(System.exePath + \"" + ttstr(Handle->CurrentTempFileName.c_str()) + L"\" + \">\");"));

	CloseHandle(Handle->CurrentTempHandle);
	InterlockedExchangePointer(&(Handle->CurrentTempHandle), INVALID_HANDLE_VALUE);

	Status = Io::DeleteFileW(Handle->CurrentTempFileName.c_str());
	if (NT_FAILED(Status))
	{
		MessageBoxW(Handle->MainWindow, L"Making Package : Successful!\nBut you must relaunch this game\nand delete \"KrkrzTempWorker.xp3\" to make the next package!!!", 
			L"KrkrExtract (Important Information!!)", MB_OK);
	}
	else
	{
		MessageBoxW(Handle->MainWindow, L"Making Package : Successful", L"KrkrExtract", MB_OK);
	}

	Handle->CurrentTempFileName = L"KrkrzTempWorker.xp3";

	return STATUS_SUCCESS;
}

//Since nekopara vol2
HRESULT WINAPI KrkrPacker::DoM2Pack_Version2(LPCWSTR lpBasePack, LPCWSTR GuessPackage, LPCWSTR OutName)
{
	NTSTATUS                Status;
	BOOL                    Result;
	GlobalData*             Handle;
	NtFileDisk              FileXP3;
	PBYTE                   pbIndex;
	ULONG                   BufferSize, CompressedSize;
	PVOID                   lpBuffer, lpCompressBuffer;
	LARGE_INTEGER           Size, Offset, BytesTransfered;
	SMyXP3IndexM2            *pXP3Index, *pIndex;
	KRKR2_XP3_DATA_HEADER   IndexHeader;
	BYTE                    FirstMagic[11] = { 0x58, 0x50, 0x33, 0x0D, 0x0A, 0x20, 0x0A, 0x1A, 0x8B, 0x67, 0x01 };
	KRKR2_XP3_HEADER        XP3Header(FirstMagic, (ULONG64)0);
	wstring                 WStrBasePath(lpBasePack);

	Handle = GlobalData::GetGlobalData();

	FileList.clear();
	IterFiles(lpBasePack);

	Status = DoM2DummyPackFirst_Version2(lpBasePack);
	if (NT_FAILED(Status))
		return Status;

	TVPExecuteScript(ttstr(L"Storages.addAutoPath(System.exePath + \"" + ttstr(Handle->CurrentTempFileName.c_str()) + L"\" + \">\");"));

	Status = FileXP3.Create(OutName);
	if (NT_FAILED(Status))
	{
		MessageBoxW(Handle->MainWindow, L"Couldn't create a handle for output xp3 file.", L"KrkrExtract", MB_OK);
		return Status;
	}

	BufferSize = 0x10000;
	CompressedSize = BufferSize;
	lpBuffer = AllocateMemoryP(BufferSize);
	lpCompressBuffer = AllocateMemoryP(CompressedSize);
	pXP3Index = (SMyXP3IndexM2 *)AllocateMemoryP(sizeof(*pXP3Index) * FileList.size());
	pIndex = pXP3Index;

	if (!lpBuffer || !lpCompressBuffer || !pXP3Index)
	{
		MessageBoxW(Handle->MainWindow, L"Insufficient memory to make package!!", L"KrkrExtract", MB_OK | MB_ICONERROR);
		FileXP3.Close();
		
		if (lpBuffer)         FreeMemoryP(lpBuffer);
		if (lpCompressBuffer) FreeMemoryP(lpCompressBuffer);
		if (pXP3Index)        FreeMemoryP(pXP3Index);
		return STATUS_INSUFFICIENT_RESOURCES;
	}

	FileXP3.Write(&XP3Header, sizeof(XP3Header), &BytesTransfered);

	Offset.QuadPart = BytesTransfered.QuadPart;
	for (ULONG i = 0; i <FileList.size(); ++pIndex, i++)
	{
		if (Handle->MainWindow)
		{
			WCHAR OutInfo[MAX_PATH];
			FormatStringW(OutInfo, PackingFormatString, i + 1, FileList.size());
			SetWindowTextW(Handle->MainWindow, OutInfo);
			Handle->SetProcess(Handle->MainWindow, (ULONG)(((float)(i + 1) / (float)FileList.size()) * 100.0));
		}

		ZeroMemory(pIndex, sizeof(*pIndex));
		*(PDWORD)(pIndex->file.Magic) = CHUNK_MAGIC_FILE;
		*(PDWORD)(pIndex->info.Magic) = CHUNK_MAGIC_INFO;
		*(PDWORD)(pIndex->time.Magic) = CHUNK_MAGIC_TIME;
		*(PDWORD)(pIndex->segm.Magic) = CHUNK_MAGIC_SEGM;
		*(PDWORD)(pIndex->adlr.Magic) = CHUNK_MAGIC_ADLR;
		*(PDWORD)(pIndex->yuzu.Magic) = Handle->M2ChunkMagic;
		pIndex->segm.ChunkSize.QuadPart = (ULONG64)0x1C; //sizeof(pIndex->segm.segm);
		pIndex->adlr.ChunkSize.QuadPart = (ULONG64)0x04;
		pIndex->info.ChunkSize.QuadPart = (ULONG64)0x58;

		if (M2NameZeroEnd)
			pIndex->file.ChunkSize.QuadPart = (ULONG64)0xB0;
		else
			pIndex->file.ChunkSize.QuadPart = (ULONG64)0xB0 - 2;
		
		pIndex->time.ChunkSize.QuadPart = (ULONG64)0x08;

		wstring DummyName = FileList[i] + L".dummy";

		ttstr FullName(L"archive://./" + ttstr(Handle->CurrentTempFileName.c_str()) + L"/");

		FullName += DummyName.c_str();

		IStream* st = TVPCreateIStream(FullName, TJS_BS_READ);
		if (st == NULL)
		{
			wstring InfoW(L"Couldn't open :\n");
			InfoW += FileList[i];
			MessageBoxW(Handle->MainWindow, InfoW.c_str(), L"KrkrExtract", MB_OK);
			FileXP3.Close();
			FreeMemoryP(lpBuffer);
			FreeMemoryP(lpCompressBuffer);
			FreeMemoryP(pXP3Index);
			return STATUS_UNSUCCESSFUL;
		}

		STATSTG t;
		st->Stat(&t, STATFLAG_DEFAULT);
		Size.QuadPart = t.cbSize.QuadPart;
		if (Size.LowPart > BufferSize)
		{
			BufferSize = Size.LowPart;
			lpBuffer = ReAllocateMemoryP(lpBuffer, BufferSize);
		}
		st->Read(lpBuffer, Size.LowPart, &BytesTransfered.LowPart);

		if (BytesTransfered.LowPart != Size.LowPart)
		{
			wstring InfoW(L"Couldn't read :\n");
			InfoW += FileList[i];
			MessageBoxW(Handle->MainWindow, InfoW.c_str(), L"KrkrExtract", MB_OK);
			FileXP3.Close();
			FreeMemoryP(lpBuffer);
			FreeMemoryP(lpCompressBuffer);
			FreeMemoryP(pXP3Index);
			return STATUS_UNSUCCESSFUL;
		}

		pIndex->segm.segm->Offset = Offset;

		wstring LowerName = ToLowerString(FileList[i].c_str());
		wstring HashName;

		GenMD5Code(LowerName.c_str(), HashName);
		pIndex->info.FileName = HashName;
		pIndex->info.FileNameLength = HashName.length();

		pIndex->yuzu.Len = FileList[i].length();
		pIndex->yuzu.Name = FileList[i];

		if (M2NameZeroEnd)
			pIndex->yuzu.ChunkSize.QuadPart = sizeof(DWORD) + sizeof(USHORT) + (pIndex->yuzu.Name.length() + 1) * 2;
		else
			pIndex->yuzu.ChunkSize.QuadPart = sizeof(DWORD) + sizeof(USHORT) + (pIndex->yuzu.Name.length()) * 2;

		pIndex->adlr.Hash = M2Hash;
		pIndex->yuzu.Hash = pIndex->adlr.Hash;

		pIndex->segm.segm->OriginalSize.LowPart = BytesTransfered.LowPart;
		pIndex->info.OriginalSize.LowPart = BytesTransfered.LowPart;

		LARGE_INTEGER EncryptOffset;

		EncryptOffset.QuadPart = 0;

		if (XP3EncryptionFlag)
			pIndex->info.EncryptedFlag = 0x80000000;
		else
			pIndex->info.EncryptedFlag = 0;

		pIndex->file.ChunkSize.QuadPart = (ULONG64)sizeof(pIndex->time.ChunkSize) + MagicLength + sizeof(pIndex->time.FileTime) +
			sizeof(pIndex->info.ArchiveSize) * 2 + sizeof(pIndex->info.EncryptedFlag) + sizeof(USHORT) + sizeof(pIndex->info.ChunkSize) + MagicLength + (pIndex->info.FileName.length() + 1) * 2 +
			sizeof(pIndex->segm.ChunkSize) + MagicLength + sizeof(pIndex->segm.segm[0].ArchiveSize) * 3 + sizeof(BOOL) +
			sizeof(pIndex->adlr.ChunkSize) + MagicLength + sizeof(pIndex->adlr.Hash);

		if ((M2NameZeroEnd && (!InfoNameZeroEnd)) || ((!M2NameZeroEnd) && InfoNameZeroEnd))
			pIndex->file.ChunkSize.QuadPart -= 2;
		else
			pIndex->file.ChunkSize.QuadPart -= 4;
		
		pIndex->segm.segm->bZlib = 0;

		pIndex->segm.segm->ArchiveSize.LowPart = BytesTransfered.LowPart;
		pIndex->info.ArchiveSize.LowPart = BytesTransfered.LowPart;
		Offset.QuadPart += BytesTransfered.QuadPart;

		FileXP3.Write(lpBuffer, BytesTransfered.LowPart, &BytesTransfered);
	}

	XP3Header.IndexOffset = Offset;

	Size.LowPart = 0;
	pIndex = pXP3Index;

	for (ULONG i = 0; i < FileList.size(); ++i, ++pIndex)
	{
		Size.LowPart += sizeof(pIndex->yuzu.Hash) * 3 + sizeof(USHORT) + (pIndex->yuzu.Name.length() + 1) * 2 + MagicLength +
			sizeof(pIndex->time.ChunkSize) + MagicLength + sizeof(pIndex->time.FileTime) +
			sizeof(pIndex->file.ChunkSize) + MagicLength +
			sizeof(pIndex->info.ArchiveSize) * 2 + sizeof(pIndex->info.EncryptedFlag) + sizeof(USHORT) + sizeof(pIndex->info.ChunkSize) + MagicLength + (pIndex->info.FileName.length() + 1) * 2 +
			sizeof(pIndex->segm.ChunkSize) + MagicLength + sizeof(pIndex->segm.segm[0].ArchiveSize) * 3 + sizeof(BOOL) +
			sizeof(pIndex->adlr.ChunkSize) + MagicLength + sizeof(pIndex->adlr.Hash);

		if ((M2NameZeroEnd && (!InfoNameZeroEnd)) || ((!M2NameZeroEnd) && InfoNameZeroEnd))
			Size.LowPart -= 2;
		else
			Size.LowPart -= 4;
	}

	if (Size.LowPart > CompressedSize)
	{
		CompressedSize = Size.LowPart;
		lpCompressBuffer = ReAllocateMemoryP(lpCompressBuffer, CompressedSize);
	}
	if (Size.LowPart * 2 > BufferSize)
	{
		BufferSize = Size.LowPart * 2;
		lpBuffer = ReAllocateMemoryP(lpBuffer, BufferSize);
	}

	// generate index to lpCompressBuffer
	pIndex = pXP3Index;
	pbIndex = (PBYTE)lpCompressBuffer;
	for (ULONG i = 0; i < FileList.size(); ++pIndex, i++)
	{
		ULONG n = sizeof(DWORD);
		CopyMemory(pbIndex, &(pIndex->yuzu.Magic), n);
		pbIndex += n;
		n = 8;
		CopyMemory(pbIndex, &(pIndex->yuzu.ChunkSize), n);
		pbIndex += n;
		n = 4;
		CopyMemory(pbIndex, &(pIndex->yuzu.Hash), n);
		pbIndex += n;
		n = sizeof(USHORT);
		CopyMemory(pbIndex, &(pIndex->yuzu.Len), n);
		pbIndex += n;

		if (M2NameZeroEnd)
		{
			n = (pIndex->yuzu.Name.length() + 1) * 2;
			CopyMemory(pbIndex, (pIndex->yuzu.Name.c_str()), n);
		}
		else
		{
			n = (pIndex->yuzu.Name.length()) * 2;
			CopyMemory(pbIndex, (pIndex->yuzu.Name.c_str()), n);
		}
		pbIndex += n;
	}

	pIndex = pXP3Index;
	for (ULONG i = 0; i < FileList.size(); ++pIndex, i++)
	{
		ULONG n = sizeof(DWORD);
		CopyMemory(pbIndex, pIndex->file.Magic, n);
		pbIndex += n;
		n = 8;
		CopyMemory(pbIndex, &(pIndex->file.ChunkSize), n);
		pbIndex += n;

		n = sizeof(DWORD);
		CopyMemory(pbIndex, pIndex->time.Magic, n);
		pbIndex += n;
		n = 8;
		CopyMemory(pbIndex, &(pIndex->time.ChunkSize), n);
		pbIndex += n;
		n = sizeof(pIndex->time.FileTime);
		CopyMemory(pbIndex, &(pIndex->time.FileTime), n);
		pbIndex += n;

		n = sizeof(DWORD);
		CopyMemory(pbIndex, pIndex->adlr.Magic, n);
		pbIndex += n;
		n = 8;
		CopyMemory(pbIndex, &(pIndex->adlr.ChunkSize), n);
		pbIndex += n;
		n = 4;
		CopyMemory(pbIndex, &(pIndex->adlr.Hash), n);
		pbIndex += n;

		n = 4;
		CopyMemory(pbIndex, &(pIndex->segm.Magic), n);
		pbIndex += n;
		n = 8;
		CopyMemory(pbIndex, &(pIndex->segm.ChunkSize), n);
		pbIndex += n;
		n = 4;
		CopyMemory(pbIndex, &(pIndex->segm.segm[0].bZlib), n);
		pbIndex += n;
		n = 8;
		CopyMemory(pbIndex, &(pIndex->segm.segm[0].Offset), n);
		pbIndex += n;
		CopyMemory(pbIndex, &(pIndex->segm.segm[0].OriginalSize), n);
		pbIndex += n;
		CopyMemory(pbIndex, &(pIndex->segm.segm[0].ArchiveSize), n);
		pbIndex += n;

		n = 4;
		CopyMemory(pbIndex, &(pIndex->info.Magic), n);
		pbIndex += n;
		n = 8;
		CopyMemory(pbIndex, &(pIndex->info.ChunkSize), n);
		pbIndex += n;
		n = 4;
		CopyMemory(pbIndex, &(pIndex->info.EncryptedFlag), n);
		pbIndex += n;
		n = 8;
		CopyMemory(pbIndex, &(pIndex->info.OriginalSize), n);
		pbIndex += n;
		CopyMemory(pbIndex, &(pIndex->info.ArchiveSize), n);
		pbIndex += n;
		n = 2;
		CopyMemory(pbIndex, &(pIndex->info.FileNameLength), n);
		pbIndex += n;

		if (InfoNameZeroEnd)
		{
			n = (pIndex->info.FileName.length() + 1) * 2;
			CopyMemory(pbIndex, pIndex->info.FileName.c_str(), n);
		}
		else
		{
			n = (pIndex->info.FileName.length()) * 2;
			CopyMemory(pbIndex, pIndex->info.FileName.c_str(), n);
		}
		pbIndex += n;
	}

	IndexHeader.bZlib = 1;
	IndexHeader.OriginalSize.QuadPart = Size.LowPart;
	IndexHeader.ArchiveSize.LowPart = BufferSize;
	BufferSize = Size.LowPart;
	compress2((PBYTE)lpBuffer, &IndexHeader.ArchiveSize.LowPart, (PBYTE)lpCompressBuffer, BufferSize, Z_BEST_COMPRESSION);
	IndexHeader.ArchiveSize.HighPart = 0;

	FileXP3.Write(&IndexHeader, sizeof(IndexHeader), &BytesTransfered);
	FileXP3.Write(lpBuffer, IndexHeader.ArchiveSize.LowPart, &BytesTransfered);
	Offset.QuadPart = 0;
	FileXP3.Seek(Offset, FILE_BEGIN);
	FileXP3.Write(&XP3Header, sizeof(XP3Header), &BytesTransfered);

	FileXP3.Close();

	FreeMemoryP(lpBuffer);
	FreeMemoryP(lpCompressBuffer);
	FreeMemoryP(pXP3Index);

	TVPExecuteScript(ttstr(L"Storages.removeAutoPath(System.exePath + \"" + ttstr(Handle->CurrentTempFileName.c_str()) + L"\" + \">\");"));

	CloseHandle(Handle->CurrentTempHandle);
	InterlockedExchangePointer(&(Handle->CurrentTempHandle), INVALID_HANDLE_VALUE);

	Status = Io::DeleteFileW(Handle->CurrentTempFileName.c_str());
	if (NT_FAILED(Status))
	{
		MessageBoxW(Handle->MainWindow, L"Making Package : Successful!\nBut you must relaunch this game\nand delete \"KrkrzTempWorker.xp3\" to make the next package!!!",
			L"KrkrExtract (Important Information!!)", MB_OK);
	}
	else
	{
		MessageBoxW(Handle->MainWindow, L"Making Package : Successful", L"KrkrExtract", MB_OK);
	}

	Handle->CurrentTempFileName = L"KrkrzTempWorker.xp3";

	return STATUS_SUCCESS;
}


NTSTATUS NTAPI KrkrPacker::DoM2Pack_SenrenBanka(LPCWSTR lpBasePack, LPCWSTR GuessPackage, LPCWSTR OutName)
{
	NTSTATUS                     Status;
	NtFileDisk                   FileXP3;
	GlobalData*                  Handle;
	PBYTE                        pbIndex;
	ULONG                        BufferSize, CompressedSize, BlockSize, BlockCompressedSize;
	PVOID                        lpBuffer, lpCompressBuffer;
	PBYTE                        lpBlock, lpBlockCompressed;
	LARGE_INTEGER                Size, Offset, BytesTransfered, ChunkSize;
	SMyXP3IndexM2                *pXP3Index, *pIndex;
	KRKR2_XP3_DATA_HEADER        IndexHeader;
	BYTE                         FirstMagic[11] = { 0x58, 0x50, 0x33, 0x0D, 0x0A, 0x20, 0x0A, 0x1A, 0x8B, 0x67, 0x01 };
	KRKR2_XP3_HEADER             XP3Header(FirstMagic, (ULONG64)0);
	wstring                      WStrBasePath(lpBasePack);
	KRKRZ_M2_Senrenbanka_HEADER  SenrenBankaHeader;

	FileList.clear();
	IterFiles(lpBasePack);

	Handle = GlobalData::GetGlobalData();
	Status = DoM2DummyPackFirst_SenrenBanka(lpBasePack);
	if (NT_FAILED(Status))
		return Status;

	if (Handle->DebugOn)
		PrintConsoleW(L"Packing files...\n");

	TVPExecuteScript(ttstr(L"Storages.addAutoPath(System.exePath + \"" + ttstr(Handle->CurrentTempFileName.c_str()) + L"\" + \">\");"));
	
	Status = FileXP3.Create(OutName);
	if (NT_FAILED(Status))
	{
		MessageBoxW(Handle->MainWindow, L"Couldn't create a handle for output xp3 file.", L"KrkrExtract", MB_OK);
		return Status;
	}

	BufferSize = 0x10000;
	CompressedSize = BufferSize;
	lpBuffer = AllocateMemoryP(BufferSize);
	lpCompressBuffer = AllocateMemoryP(CompressedSize);
	pXP3Index = (SMyXP3IndexM2 *)AllocateMemoryP(sizeof(*pXP3Index) * FileList.size());
	pIndex = pXP3Index;

	if (!lpBuffer || !lpCompressBuffer || !pXP3Index)
	{
		MessageBoxW(Handle->MainWindow, L"Insufficient memory to make package!!", L"KrkrExtract", MB_OK | MB_ICONERROR);
		FileXP3.Close();

		if (lpBuffer)         FreeMemoryP(lpBuffer);
		if (lpCompressBuffer) FreeMemoryP(lpCompressBuffer);
		if (pXP3Index)        FreeMemoryP(pXP3Index);
		return STATUS_INSUFFICIENT_RESOURCES;
	}

	FileXP3.Write(&XP3Header, sizeof(XP3Header), &BytesTransfered);

	Offset.QuadPart = BytesTransfered.QuadPart;
	for (ULONG i = 0; i <FileList.size(); ++pIndex, i++)
	{
		WCHAR OutInfo[MAX_PATH];
		RtlZeroMemory(OutInfo, countof(OutInfo) * sizeof(WCHAR));
		FormatStringW(OutInfo, PackingFormatString, i + 1, FileList.size());
		SetWindowTextW(Handle->MainWindow, OutInfo);
		Handle->SetProcess(Handle->MainWindow, (ULONG)(((float)(i + 1) / (float)FileList.size()) * 100.0));

		ZeroMemory(pIndex, sizeof(*pIndex));
		*(PDWORD)(pIndex->file.Magic) = CHUNK_MAGIC_FILE;
		*(PDWORD)(pIndex->info.Magic) = CHUNK_MAGIC_INFO;
		*(PDWORD)(pIndex->time.Magic) = CHUNK_MAGIC_TIME;
		*(PDWORD)(pIndex->segm.Magic) = CHUNK_MAGIC_SEGM;
		*(PDWORD)(pIndex->adlr.Magic) = CHUNK_MAGIC_ADLR;
		pIndex->segm.ChunkSize.QuadPart = (ULONG64)0x1C;
		pIndex->adlr.ChunkSize.QuadPart = (ULONG64)0x04;
		pIndex->info.ChunkSize.QuadPart = (ULONG64)0x58;
		pIndex->file.ChunkSize.QuadPart = (ULONG64)0xB0;
		pIndex->time.ChunkSize.QuadPart = (ULONG64)0x08;

		wstring DummyName = FileList[i] + L".dummy";

		ttstr FullName(L"archive://./" + ttstr(Handle->CurrentTempFileName.c_str()) + "/");

		FullName += DummyName.c_str();

		IStream* Stream = TVPCreateIStream(FullName, TJS_BS_READ);
		if (Stream == NULL)
		{
			if (Handle->DebugOn)
				PrintConsoleW(L"Couldn't open %s\n", DummyName.c_str());

			wstring InfoW(L"Couldn't open :\n");
			InfoW += FileList[i];
			MessageBoxW(Handle->MainWindow, InfoW.c_str(), L"KrkrExtract", MB_OK);
			FileXP3.Close();
			FreeMemoryP(lpBuffer);
			FreeMemoryP(lpCompressBuffer);
			FreeMemoryP(pXP3Index);
			return STATUS_UNSUCCESSFUL;
		}

		STATSTG t;
		Stream->Stat(&t, STATFLAG_DEFAULT);
		
		Size.QuadPart = t.cbSize.QuadPart;
		if (Size.LowPart > BufferSize)
		{
			BufferSize = Size.LowPart;
			lpBuffer = ReAllocateMemoryP(lpBuffer, BufferSize);
		}
		
		Stream->Read(lpBuffer, Size.LowPart, &BytesTransfered.LowPart);
		
		if (BytesTransfered.LowPart != Size.LowPart)
		{
			wstring InfoW(L"Couldn't read :\n");
			InfoW += FileList[i];
			MessageBoxW(Handle->MainWindow, InfoW.c_str(), L"KrkrExtract", MB_OK);
			FileXP3.Close();
			FreeMemoryP(lpBuffer);
			FreeMemoryP(lpCompressBuffer);
			FreeMemoryP(pXP3Index);
			return STATUS_UNSUCCESSFUL;
		}
		
		pIndex->segm.segm->Offset = Offset;

		wstring LowerName = ToLowerString(FileList[i].c_str());
		wstring HashName;

		GenMD5Code(LowerName.c_str(), SenrenBankaInfo.ProductName, HashName);
		pIndex->info.FileName = HashName;
		pIndex->info.FileNameLength = HashName.length();

		pIndex->yuzu.Len = FileList[i].length();
		pIndex->yuzu.Name = FileList[i];

		if (M2NameZeroEnd)
			pIndex->yuzu.ChunkSize.QuadPart = sizeof(DWORD) + sizeof(USHORT) + (pIndex->yuzu.Name.length() + 1) * 2;
		else
			pIndex->yuzu.ChunkSize.QuadPart = sizeof(DWORD) + sizeof(USHORT) + (pIndex->yuzu.Name.length()) * 2;

		pIndex->adlr.Hash = M2Hash;
		pIndex->yuzu.Hash = pIndex->adlr.Hash;

		pIndex->segm.segm->OriginalSize.LowPart = BytesTransfered.LowPart;
		pIndex->info.OriginalSize.LowPart = BytesTransfered.LowPart;

		LARGE_INTEGER EncryptOffset;

		EncryptOffset.QuadPart = 0;

		if (XP3EncryptionFlag)
			pIndex->info.EncryptedFlag = 0x80000000;
		else
			pIndex->info.EncryptedFlag = 0;

		PackChunkList.push_back(*pIndex);

		pIndex->file.ChunkSize.QuadPart = (ULONG64)sizeof(pIndex->time.ChunkSize) + MagicLength + sizeof(pIndex->time.FileTime) +
			sizeof(pIndex->info.ArchiveSize) * 2 + sizeof(pIndex->info.EncryptedFlag) + sizeof(USHORT) + sizeof(pIndex->info.ChunkSize) + MagicLength + (pIndex->info.FileName.length() + 1) * 2 +
			sizeof(pIndex->segm.ChunkSize) + MagicLength + sizeof(pIndex->segm.segm[0].ArchiveSize) * 3 + sizeof(BOOL) +
			sizeof(pIndex->adlr.ChunkSize) + MagicLength + sizeof(pIndex->adlr.Hash);

		pIndex->segm.segm->bZlib = 0;

		pIndex->segm.segm->ArchiveSize.LowPart = BytesTransfered.LowPart;
		pIndex->info.ArchiveSize.LowPart = BytesTransfered.LowPart;
		Offset.QuadPart += BytesTransfered.QuadPart;

		FileXP3.Write(lpBuffer, BytesTransfered.LowPart, &BytesTransfered);
	}

	Size.LowPart = 0;
	pIndex = pXP3Index;

	for (ULONG i = 0; i < FileList.size(); ++i, ++pIndex)
	{
		Size.LowPart += 
			sizeof(pIndex->time.ChunkSize) + MagicLength + sizeof(pIndex->time.FileTime) +
			sizeof(pIndex->file.ChunkSize) + MagicLength +
			sizeof(pIndex->info.ArchiveSize) * 2 + sizeof(pIndex->info.EncryptedFlag) + sizeof(USHORT) + sizeof(pIndex->info.ChunkSize) + MagicLength + (pIndex->info.FileName.length() + 1) * 2 +
			sizeof(pIndex->segm.ChunkSize) + MagicLength + sizeof(pIndex->segm.segm[0].ArchiveSize) * 3 + sizeof(BOOL) +
			sizeof(pIndex->adlr.ChunkSize) + MagicLength + sizeof(pIndex->adlr.Hash);
	}

	Size.LowPart += sizeof(KRKRZ_M2_Senrenbanka_HEADER);

	if (Size.LowPart > CompressedSize)
	{
		CompressedSize = Size.LowPart;
		lpCompressBuffer = ReAllocateMemoryP(lpCompressBuffer, CompressedSize);
	}
	if (Size.LowPart * 2 > BufferSize)
	{
		BufferSize = Size.LowPart * 2;
		lpBuffer = ReAllocateMemoryP(lpBuffer, BufferSize);
	}

	// generate index to lpCompressBuffer
	
	lpBlock = (PBYTE)AllocateMemoryP(FileList.size() * sizeof(KRKRZ_XP3_INDEX_CHUNK_Yuzu2));
	lpBlockCompressed = (PBYTE)AllocateMemoryP(FileList.size() * sizeof(KRKRZ_XP3_INDEX_CHUNK_Yuzu2) * 2);

	BlockSize = 0;
	for (ULONG i = 0; i < FileList.size(); i++)
	{
		RtlCopyMemory((lpBlock + BlockSize), &M2SubChunkMagic, 4);
		BlockSize += 4;
		ChunkSize.QuadPart = sizeof(DWORD) + sizeof(USHORT) + (StrLengthW(FileList[i].c_str()) + 1) * 2;
		RtlCopyMemory((lpBlock + BlockSize), &(ChunkSize.QuadPart), 8);
		BlockSize += 8;
		RtlCopyMemory((lpBlock + BlockSize), &M2Hash, 4);
		BlockSize += 4;

		USHORT NameLength = (USHORT)StrLengthW(FileList[i].c_str());
		RtlCopyMemory((lpBlock + BlockSize), &NameLength, 2);
		BlockSize += 2;
		RtlCopyMemory((lpBlock + BlockSize), FileList[i].c_str(), (FileList[i].length() + 1) * 2);
		BlockSize += (FileList[i].length() + 1) * 2;
	}


	SenrenBankaHeader.OriginalSize = BlockSize;
	SenrenBankaHeader.Magic = Handle->M2ChunkMagic;
	SenrenBankaHeader.LengthOfProduct = SenrenBankaInfo.LengthOfProduct;
	StrCopyW(SenrenBankaHeader.ProductName, SenrenBankaInfo.ProductName);
	SenrenBankaHeader.ArchiveSize = FileList.size() * sizeof(KRKRZ_XP3_INDEX_CHUNK_Yuzu2) * 2;
	compress2(lpBlockCompressed, &SenrenBankaHeader.ArchiveSize, lpBlock, BlockSize, Z_BEST_COMPRESSION);
	FreeMemoryP(lpBlock);
	SenrenBankaHeader.ChunkSize = SenrenBankaInfo.ChunkSize;
	SenrenBankaHeader.Offset = Offset;

	FileXP3.Write(lpBlockCompressed, SenrenBankaHeader.ArchiveSize);
	FreeMemoryP(lpBlockCompressed);

	Offset.QuadPart += SenrenBankaHeader.ArchiveSize;
	XP3Header.IndexOffset = Offset;

	BlockSize = 0;

	*(PDWORD)(PBYTE)lpCompressBuffer = Handle->M2ChunkMagic;
	BlockSize += 4;
	*(PULONG64)((PBYTE)lpCompressBuffer + BlockSize) = SenrenBankaHeader.ChunkSize.QuadPart;
	BlockSize += 8;
	*(PULONG64)((PBYTE)lpCompressBuffer + BlockSize) = SenrenBankaHeader.Offset.QuadPart;
	BlockSize += 8;
	*(PDWORD)((PBYTE)lpCompressBuffer + BlockSize) = SenrenBankaHeader.OriginalSize;
	BlockSize += 4;
	*(PDWORD)((PBYTE)lpCompressBuffer + BlockSize) = SenrenBankaHeader.ArchiveSize;
	BlockSize += 4;
	*(PUSHORT)((PBYTE)lpCompressBuffer + BlockSize) = SenrenBankaHeader.LengthOfProduct;
	BlockSize += 2;
	RtlCopyMemory(((PBYTE)lpCompressBuffer + BlockSize), SenrenBankaHeader.ProductName, (StrLengthW(SenrenBankaHeader.ProductName) + 1) * 2);
	BlockSize += (StrLengthW(SenrenBankaHeader.ProductName) + 1) * 2;

	pIndex = pXP3Index;
	pbIndex = (PBYTE)lpCompressBuffer + BlockSize;
	for (ULONG i = 0; i < FileList.size(); ++pIndex, i++)
	{
		ULONG n = sizeof(DWORD);
		CopyMemory(pbIndex, pIndex->file.Magic, n);
		pbIndex += n;
		n = 8;
		CopyMemory(pbIndex, &(pIndex->file.ChunkSize), n);
		pbIndex += n;

		n = sizeof(DWORD);
		CopyMemory(pbIndex, pIndex->time.Magic, n);
		pbIndex += n;
		n = 8;
		CopyMemory(pbIndex, &(pIndex->time.ChunkSize), n);
		pbIndex += n;
		n = sizeof(pIndex->time.FileTime);
		CopyMemory(pbIndex, &(pIndex->time.FileTime), n);
		pbIndex += n;

		n = sizeof(DWORD);
		CopyMemory(pbIndex, pIndex->adlr.Magic, n);
		pbIndex += n;
		n = 8;
		CopyMemory(pbIndex, &(pIndex->adlr.ChunkSize), n);
		pbIndex += n;
		n = 4;
		CopyMemory(pbIndex, &(pIndex->adlr.Hash), n);
		pbIndex += n;

		n = 4;
		CopyMemory(pbIndex, &(pIndex->segm.Magic), n);
		pbIndex += n;
		n = 8;
		CopyMemory(pbIndex, &(pIndex->segm.ChunkSize), n);
		pbIndex += n;
		n = 4;
		CopyMemory(pbIndex, &(pIndex->segm.segm[0].bZlib), n);
		pbIndex += n;
		n = 8;
		CopyMemory(pbIndex, &(pIndex->segm.segm[0].Offset), n);
		pbIndex += n;
		CopyMemory(pbIndex, &(pIndex->segm.segm[0].OriginalSize), n);
		pbIndex += n;
		CopyMemory(pbIndex, &(pIndex->segm.segm[0].ArchiveSize), n);
		pbIndex += n;

		n = 4;
		CopyMemory(pbIndex, &(pIndex->info.Magic), n);
		pbIndex += n;
		n = 8;
		CopyMemory(pbIndex, &(pIndex->info.ChunkSize), n);
		pbIndex += n;
		n = 4;
		CopyMemory(pbIndex, &(pIndex->info.EncryptedFlag), n);
		pbIndex += n;
		n = 8;
		CopyMemory(pbIndex, &(pIndex->info.OriginalSize), n);
		pbIndex += n;
		CopyMemory(pbIndex, &(pIndex->info.ArchiveSize), n);
		pbIndex += n;
		n = 2;
		CopyMemory(pbIndex, &(pIndex->info.FileNameLength), n);
		pbIndex += n;

		n = (pIndex->info.FileName.length() + 1) * 2;
		CopyMemory(pbIndex, pIndex->info.FileName.c_str(), n);

		pbIndex += n;
	}

	IndexHeader.bZlib = 1;
	IndexHeader.OriginalSize.QuadPart = Size.LowPart + BlockSize - sizeof(KRKRZ_M2_Senrenbanka_HEADER);
	BufferSize = Size.LowPart + BlockSize - sizeof(KRKRZ_M2_Senrenbanka_HEADER);
	IndexHeader.ArchiveSize.LowPart = BufferSize;
	compress2((PBYTE)lpBuffer, &IndexHeader.ArchiveSize.LowPart, (PBYTE)lpCompressBuffer, BufferSize, Z_BEST_COMPRESSION);
	IndexHeader.ArchiveSize.HighPart = 0;

	FileXP3.Write(&IndexHeader, sizeof(IndexHeader), &BytesTransfered);
	FileXP3.Write(lpBuffer, IndexHeader.ArchiveSize.LowPart, &BytesTransfered);
	Offset.QuadPart = 0;
	FileXP3.Seek(Offset, FILE_BEGIN);
	FileXP3.Write(&XP3Header, sizeof(XP3Header), &BytesTransfered);

	FileXP3.Close();

	FreeMemoryP(lpBuffer);
	FreeMemoryP(lpCompressBuffer);
	FreeMemoryP(pXP3Index);

	TVPExecuteScript(ttstr(L"Storages.removeAutoPath(System.exePath + \"" + ttstr(Handle->CurrentTempFileName.c_str()) + L"\" + \">\");"));

	CloseHandle(Handle->CurrentTempHandle);
	InterlockedExchangePointer(&(Handle->CurrentTempHandle), INVALID_HANDLE_VALUE);
	
	Status = Io::DeleteFileW(Handle->CurrentTempFileName.c_str());
	if (NT_FAILED(Status))
	{
		MessageBoxW(Handle->MainWindow,
			(L"Making Package : Successful!\nBut you must relaunch this game\nand delete \"" + Handle->CurrentTempFileName + L"\" to make the next package!!!").c_str(),
			L"KrkrExtract (Important Information!!)", MB_OK);
	}
	else
	{
		MessageBoxW(Handle->MainWindow, L"Making Package : Successful", L"KrkrExtract", MB_OK);
	}

	Handle->CurrentTempFileName = L"KrkrzTempWorker.xp3";
	return STATUS_SUCCESS;
}

NTSTATUS NTAPI KrkrPacker::DoM2DummyPackFirst_SenrenBanka(LPCWSTR lpBasePack)
{
	NTSTATUS                     Status;
	GlobalData*                  Handle;
	BOOL                         Result;
	NtFileDisk                   File, FileXP3;
	PBYTE                        pbIndex;
	ULONG                        BufferSize, CompressedSize, BlockSize, BlockCompressedSize;
	PVOID                        lpBuffer, lpCompressBuffer;
	PBYTE                        lpBlock, lpBlockCompressed;
	LARGE_INTEGER                Size, Offset, BytesTransfered, ChunkSize;
	SMyXP3IndexM2                *pXP3Index, *pIndex;
	KRKR2_XP3_DATA_HEADER        IndexHeader;
	BYTE                         FirstMagic[11] = { 0x58, 0x50, 0x33, 0x0D, 0x0A, 0x20, 0x0A, 0x1A, 0x8B, 0x67, 0x01 };
	KRKR2_XP3_HEADER             XP3Header(FirstMagic, (ULONG64)0);
	wstring                      WStrBasePath(lpBasePack);
	KRKRZ_M2_Senrenbanka_HEADER  SenrenBankaHeader;
	WCHAR                        CurTempFileName[MAX_PATH];
	ULONG64                      RandNum;

	Handle = GlobalData::GetGlobalData();

	RandNum = genrand64_int64();
	RtlZeroMemory(CurTempFileName, countof(CurTempFileName) * 2);
	FormatStringW(CurTempFileName, L"KrkrzTempWorker_%08x%08x.xp3", HIDWORD(RandNum), LODWORD(RandNum));
	Handle->CurrentTempFileName = CurTempFileName;

	RtlZeroMemory(&SenrenBankaHeader, sizeof(SenrenBankaHeader));
	Status = FileXP3.Create(Handle->CurrentTempFileName.c_str());


	if (NT_FAILED(Status))
	{
		MessageBoxW(NULL, L"Couldn't create a handle for temporary output file.", L"KrkrExtract", MB_OK);
		return Status;
	}
	
	BufferSize = 0x10000;
	CompressedSize = BufferSize;
	lpBuffer = AllocateMemoryP(BufferSize);
	lpCompressBuffer = AllocateMemoryP(CompressedSize);
	pXP3Index = (SMyXP3IndexM2 *)AllocateMemoryP(sizeof(*pXP3Index) * FileList.size());
	pIndex = pXP3Index;

	if (!lpBuffer || !lpCompressBuffer || !pXP3Index)
	{
		MessageBoxW(Handle->MainWindow, L"Insufficient memory to make package!!", L"KrkrExtract", MB_OK | MB_ICONERROR);
		FileXP3.Close();

		if(lpBuffer)         FreeMemoryP(lpBuffer);
		if(lpCompressBuffer) FreeMemoryP(lpCompressBuffer);
		if(pXP3Index)        FreeMemoryP(pXP3Index);
		return STATUS_INSUFFICIENT_RESOURCES;
	}

	FileXP3.Write(&XP3Header, sizeof(XP3Header), &BytesTransfered);

	Offset.QuadPart = BytesTransfered.QuadPart;

	if (FileList.size() == 0)
	{
		MessageBoxW(NULL, L"No File to be packed", L"KrkrExtract", MB_OK);
		FileXP3.Close();
		FreeMemoryP(lpBuffer);
		FreeMemoryP(lpCompressBuffer);
		FreeMemoryP(pXP3Index);
		return STATUS_UNSUCCESSFUL;
	}
	
	for (ULONG i = 0; i < FileList.size(); ++pIndex, i++)
	{

		ZeroMemory(pIndex, sizeof(*pIndex));
		*(PDWORD)(pIndex->file.Magic) = CHUNK_MAGIC_FILE;
		*(PDWORD)(pIndex->info.Magic) = CHUNK_MAGIC_INFO;
		*(PDWORD)(pIndex->time.Magic) = CHUNK_MAGIC_TIME;
		*(PDWORD)(pIndex->segm.Magic) = CHUNK_MAGIC_SEGM;
		*(PDWORD)(pIndex->adlr.Magic) = CHUNK_MAGIC_ADLR;
		pIndex->segm.ChunkSize.QuadPart = (ULONG64)0x1C;
		pIndex->adlr.ChunkSize.QuadPart = (ULONG64)0x04;
		pIndex->info.ChunkSize.QuadPart = (ULONG64)0x58;
		pIndex->file.ChunkSize.QuadPart = (ULONG64)0xB0;
		pIndex->time.ChunkSize.QuadPart = (ULONG64)0x08;

		wstring FullName = wstring(lpBasePack) + L"\\";
		FullName += FileList[i];
		Status = File.Open(FullName.c_str());

		if (NT_FAILED(Status))
		{
			wstring InfoW(L"Dummy write :\n[Failed to open]\n");
			InfoW += FullName;
			MessageBoxW(Handle->MainWindow, InfoW.c_str(), L"KrkrExtract", MB_OK);
			FileXP3.Close();
			FreeMemoryP(lpBuffer);
			FreeMemoryP(lpCompressBuffer);
			FreeMemoryP(pXP3Index);
			return Status;
		}

		File.GetSize(&Size);
		if (Size.LowPart > BufferSize)
		{
			BufferSize = Size.LowPart;
			lpBuffer = ReAllocateMemoryP(lpBuffer, BufferSize);
		}

		File.Read(lpBuffer, Size.LowPart, &BytesTransfered);

		if (BytesTransfered.LowPart != Size.LowPart)
		{
			wstring InfoW(L"Dummy write :\n[Failed to read]\n");
			InfoW += FullName;
			MessageBoxW(Handle->MainWindow, InfoW.c_str(), L"KrkrExtract", MB_OK);
			File.Close();
			FileXP3.Close();
			FreeMemoryP(lpBuffer);
			FreeMemoryP(lpCompressBuffer);
			FreeMemoryP(pXP3Index);
			return STATUS_IO_DEVICE_ERROR;
		}

		pIndex->segm.segm->Offset = Offset;

		wstring DummyName, DummyLowerName, HashName;

		DummyName = FileList[i] + L".dummy";
		DummyLowerName = ToLowerString(DummyName.c_str());

		GenMD5Code(DummyLowerName.c_str(), SenrenBankaInfo.ProductName, HashName);
		pIndex->info.FileName = HashName;
		pIndex->info.FileNameLength = HashName.length();

		pIndex->adlr.Hash = M2Hash;

		pIndex->segm.segm->OriginalSize.LowPart = BytesTransfered.LowPart;
		pIndex->info.OriginalSize.LowPart = BytesTransfered.LowPart;

		FILETIME Time1, Time2;
		GetFileTime(File.GetHandle(), &(pIndex->time.FileTime), &Time1, &Time2);
		File.Close();
		LARGE_INTEGER EncryptOffset;

		EncryptOffset.QuadPart = 0;

		if (XP3EncryptionFlag)
			pIndex->info.EncryptedFlag = 0x80000000;
		else
			pIndex->info.EncryptedFlag = 0;

		PackChunkList.push_back(*pIndex);

		pIndex->file.ChunkSize.QuadPart = (ULONG64)sizeof(pIndex->time.ChunkSize) + MagicLength + sizeof(pIndex->time.FileTime) +
				sizeof(pIndex->info.ArchiveSize) * 2 + sizeof(pIndex->info.EncryptedFlag) + sizeof(USHORT) + sizeof(pIndex->info.ChunkSize) + MagicLength + (pIndex->info.FileName.length() + 1) * 2 +
				sizeof(pIndex->segm.ChunkSize) + MagicLength + sizeof(pIndex->segm.segm[0].ArchiveSize) * 3 + sizeof(BOOL) +
				sizeof(pIndex->adlr.ChunkSize) + MagicLength + sizeof(pIndex->adlr.Hash);

		pIndex->segm.segm->bZlib = 0;

		pIndex->segm.segm->ArchiveSize.LowPart = BytesTransfered.LowPart;
		pIndex->info.ArchiveSize.LowPart = BytesTransfered.LowPart;
		Offset.QuadPart += BytesTransfered.QuadPart;

		FileXP3.Write(lpBuffer, BytesTransfered.LowPart, &BytesTransfered);
	}
	
	// generate index, calculate index size first
	Size.LowPart = 0;
	pIndex = pXP3Index;

	for (ULONG i = 0; i < FileList.size(); ++i, ++pIndex)
	{
		Size.LowPart += 
			sizeof(pIndex->time.ChunkSize) + MagicLength + sizeof(pIndex->time.FileTime) +
			sizeof(pIndex->file.ChunkSize) + MagicLength +
			sizeof(pIndex->info.ArchiveSize) * 2 + sizeof(pIndex->info.EncryptedFlag) + sizeof(USHORT) + sizeof(pIndex->info.ChunkSize) + MagicLength + (pIndex->info.FileName.length() + 1) * 2 +
			sizeof(pIndex->segm.ChunkSize) + MagicLength + sizeof(pIndex->segm.segm[0].ArchiveSize) * 3 + sizeof(BOOL) +
			sizeof(pIndex->adlr.ChunkSize) + MagicLength + sizeof(pIndex->adlr.Hash);
	}
	
	Size.LowPart += sizeof(KRKRZ_M2_Senrenbanka_HEADER);

	if (Size.LowPart > CompressedSize)
	{
		CompressedSize = Size.LowPart;
		lpCompressBuffer = ReAllocateMemoryP(lpCompressBuffer, CompressedSize);
	}
	if (Size.LowPart * 2 > BufferSize)
	{
		BufferSize = Size.LowPart * 2;
		lpBuffer = ReAllocateMemoryP(lpBuffer, BufferSize);
	}

	// generate index to lpCompressBuffer
	lpBlock = (PBYTE)AllocateMemoryP(FileList.size() * sizeof(KRKRZ_XP3_INDEX_CHUNK_Yuzu2));
	lpBlockCompressed = (PBYTE)AllocateMemoryP(FileList.size() * sizeof(KRKRZ_XP3_INDEX_CHUNK_Yuzu2) * 2);

	BlockSize = 0;
	for (ULONG i = 0; i < FileList.size(); i++)
	{
		RtlCopyMemory((lpBlock + BlockSize), &M2SubChunkMagic, 4);
		BlockSize += 4;
		ChunkSize.QuadPart = sizeof(DWORD) + sizeof(USHORT) + (StrLengthW(FileList[i].c_str()) + StrLengthW(L".dummy") + 1) * 2;
		RtlCopyMemory((lpBlock + BlockSize), &(ChunkSize.QuadPart), 8);
		BlockSize += 8;
		RtlCopyMemory((lpBlock + BlockSize), &M2Hash, 4);
		BlockSize += 4;

		USHORT NameLength = (USHORT)(StrLengthW(FileList[i].c_str()) + StrLengthW(L".dummy"));
		RtlCopyMemory((lpBlock + BlockSize), &NameLength, 2);
		BlockSize += 2;
		RtlCopyMemory((lpBlock + BlockSize), FileList[i].c_str(), FileList[i].length() * 2);
		BlockSize += FileList[i].length() * 2;
		RtlCopyMemory((lpBlock + BlockSize), L".dummy", (StrLengthW(L".dummy") + 1) * 2);
		BlockSize += (StrLengthW(L".dummy") + 1) * 2;
	}
	
	SenrenBankaHeader.OriginalSize = BlockSize;
	SenrenBankaHeader.Magic = Handle->M2ChunkMagic;
	SenrenBankaHeader.LengthOfProduct = SenrenBankaInfo.LengthOfProduct;
	StrCopyW(SenrenBankaHeader.ProductName, SenrenBankaInfo.ProductName);
	SenrenBankaHeader.ArchiveSize = FileList.size() * sizeof(KRKRZ_XP3_INDEX_CHUNK_Yuzu2) * 2;
	compress2(lpBlockCompressed, &SenrenBankaHeader.ArchiveSize, lpBlock, BlockSize, Z_BEST_COMPRESSION);
	FreeMemoryP(lpBlock);
	SenrenBankaHeader.ChunkSize = SenrenBankaInfo.ChunkSize;
	SenrenBankaHeader.Offset   = Offset;

	FileXP3.Write(lpBlockCompressed, SenrenBankaHeader.ArchiveSize);
	FreeMemoryP(lpBlockCompressed);

	Offset.QuadPart += SenrenBankaHeader.ArchiveSize;
	XP3Header.IndexOffset = Offset;

	BlockSize = 0;
	
	*(PDWORD)(PBYTE)lpCompressBuffer = Handle->M2ChunkMagic;
	BlockSize += 4;
	*(PULONG64)((PBYTE)lpCompressBuffer + BlockSize) = SenrenBankaHeader.ChunkSize.QuadPart;
	BlockSize += 8;
	*(PULONG64)((PBYTE)lpCompressBuffer + BlockSize) = SenrenBankaHeader.Offset.QuadPart;
	BlockSize += 8;
	*(PDWORD)((PBYTE)lpCompressBuffer + BlockSize) = SenrenBankaHeader.OriginalSize;
	BlockSize += 4;
	*(PDWORD)((PBYTE)lpCompressBuffer + BlockSize) = SenrenBankaHeader.ArchiveSize;
	BlockSize += 4;
	*(PUSHORT)((PBYTE)lpCompressBuffer + BlockSize) = SenrenBankaHeader.LengthOfProduct;
	BlockSize += 2;
	RtlCopyMemory(((PBYTE)lpCompressBuffer + BlockSize), SenrenBankaHeader.ProductName, (StrLengthW(SenrenBankaHeader.ProductName) + 1) * 2);
	BlockSize += (StrLengthW(SenrenBankaHeader.ProductName) + 1) * 2;
	
	
	pIndex = pXP3Index;
	pbIndex = (PBYTE)lpCompressBuffer + BlockSize;
	for (ULONG i = 0; i < FileList.size(); ++pIndex, i++)
	{
		ULONG n = sizeof(DWORD);
		CopyMemory(pbIndex, pIndex->file.Magic, n);
		pbIndex += n;
		n = 8;
		CopyMemory(pbIndex, &(pIndex->file.ChunkSize), n);
		pbIndex += n;

		n = sizeof(DWORD);
		CopyMemory(pbIndex, pIndex->time.Magic, n);
		pbIndex += n;
		n = 8;
		CopyMemory(pbIndex, &(pIndex->time.ChunkSize), n);
		pbIndex += n;
		n = sizeof(pIndex->time.FileTime);
		CopyMemory(pbIndex, &(pIndex->time.FileTime), n);
		pbIndex += n;

		n = sizeof(DWORD);
		CopyMemory(pbIndex, pIndex->adlr.Magic, n);
		pbIndex += n;
		n = 8;
		CopyMemory(pbIndex, &(pIndex->adlr.ChunkSize), n);
		pbIndex += n;
		n = 4;
		CopyMemory(pbIndex, &(pIndex->adlr.Hash), n);
		pbIndex += n;

		n = 4;
		CopyMemory(pbIndex, &(pIndex->segm.Magic), n);
		pbIndex += n;
		n = 8;
		CopyMemory(pbIndex, &(pIndex->segm.ChunkSize), n);
		pbIndex += n;
		n = 4;
		CopyMemory(pbIndex, &(pIndex->segm.segm[0].bZlib), n);
		pbIndex += n;
		n = 8;
		CopyMemory(pbIndex, &(pIndex->segm.segm[0].Offset), n);
		pbIndex += n;
		CopyMemory(pbIndex, &(pIndex->segm.segm[0].OriginalSize), n);
		pbIndex += n;
		CopyMemory(pbIndex, &(pIndex->segm.segm[0].ArchiveSize), n);
		pbIndex += n;

		n = 4;
		CopyMemory(pbIndex, &(pIndex->info.Magic), n);
		pbIndex += n;
		n = 8;
		CopyMemory(pbIndex, &(pIndex->info.ChunkSize), n);
		pbIndex += n;
		n = 4;
		CopyMemory(pbIndex, &(pIndex->info.EncryptedFlag), n);
		pbIndex += n;
		n = 8;
		CopyMemory(pbIndex, &(pIndex->info.OriginalSize), n);
		pbIndex += n;
		CopyMemory(pbIndex, &(pIndex->info.ArchiveSize), n);
		pbIndex += n;
		n = 2;
		CopyMemory(pbIndex, &(pIndex->info.FileNameLength), n);
		pbIndex += n;

		n = (pIndex->info.FileName.length() + 1) * 2;
		CopyMemory(pbIndex, pIndex->info.FileName.c_str(), n);

		pbIndex += n;
	}

	IndexHeader.bZlib = 1;
	IndexHeader.OriginalSize.QuadPart = Size.LowPart + BlockSize - sizeof(KRKRZ_M2_Senrenbanka_HEADER);
	BufferSize = Size.LowPart + BlockSize - sizeof(KRKRZ_M2_Senrenbanka_HEADER);
	IndexHeader.ArchiveSize.LowPart = BufferSize;
	compress2((PBYTE)lpBuffer, &IndexHeader.ArchiveSize.LowPart, (PBYTE)lpCompressBuffer, BufferSize, Z_BEST_COMPRESSION);
	IndexHeader.ArchiveSize.HighPart = 0;

	FileXP3.Write(&IndexHeader, sizeof(IndexHeader), &BytesTransfered);
	FileXP3.Write(lpBuffer, IndexHeader.ArchiveSize.LowPart, &BytesTransfered);
	Offset.QuadPart = 0;
	FileXP3.Seek(Offset, FILE_BEGIN);
	FileXP3.Write(&XP3Header, sizeof(XP3Header), &BytesTransfered);
	FileXP3.Close();

	FreeMemoryP(lpBuffer);
	FreeMemoryP(lpCompressBuffer);
	FreeMemoryP(pXP3Index);

	
	return STATUS_SUCCESS;
}

/*********************************************************************/


DWORD WINAPI PackerThread(PVOID lpParam)
{
	NTSTATUS      Status;
	GlobalData*   Handle;
	WCHAR         BasePackName[MAX_PATH];
	WCHAR         GuessPackName[MAX_PATH];
	WCHAR         OutPackName[MAX_PATH];

	Handle = GlobalData::GetGlobalData();

	RtlZeroMemory(GuessPackName, countof(GuessPackName) * sizeof(WCHAR));
	RtlZeroMemory(BasePackName,  countof(BasePackName)  * sizeof(WCHAR));
	RtlZeroMemory(OutPackName,   countof(OutPackName)   * sizeof(WCHAR));

	Handle->GetGuessPack(GuessPackName, MAX_PATH);
	Handle->GetOutputPack(OutPackName,  MAX_PATH);
	Handle->GetFolder(BasePackName,     MAX_PATH);

	Status = LocalKrkrPacker.DetactPackFormat(GuessPackName);
	if (NT_FAILED(Status))
	{
		LocalKrkrPacker.InternalReset();
		return Status;
	}

	switch (LocalKrkrPacker.KrkrPackType)
	{
	case PackInfo::NormalPack:
		Status = LocalKrkrPacker.DoNormalPack(BasePackName, GuessPackName, OutPackName);
		break;

	case PackInfo::NormalPack_NoExporter:
		Status = LocalKrkrPacker.DoNormalPackEx(BasePackName, GuessPackName, OutPackName);
		break;

	case PackInfo::KrkrZ:
		Status = LocalKrkrPacker.DoM2Pack(BasePackName, GuessPackName, OutPackName);
		break;

	case PackInfo::KrkrZ_V2:
		Status = LocalKrkrPacker.DoM2Pack_Version2(BasePackName, GuessPackName, OutPackName);
		break;

	case PackInfo::KrkrZ_SenrenBanka:
		Status = LocalKrkrPacker.DoM2Pack_SenrenBanka(BasePackName, GuessPackName, OutPackName);
		break;
	}

	LocalKrkrPacker.InternalReset();
	return Status;
}

HANDLE NTAPI StartPacker()
{
	NTSTATUS     Status;
	GlobalData*  Handle;
	WCHAR        BasePack[MAX_PATH];

	Handle = GlobalData::GetGlobalData();
	
	RtlZeroMemory(BasePack, countof(BasePack) * sizeof(WCHAR));
	Handle->DisableAll(Handle->MainWindow);

	LocalKrkrPacker.InternalReset();
	if (Handle->isRunning || Handle->WorkerThread != INVALID_HANDLE_VALUE)
	{
		MessageBoxW(NULL, L"Another task is under processing!", L"KrkrExtract", MB_OK);
		LocalKrkrPacker.InternalReset();
		return INVALID_HANDLE_VALUE;
	}

	GlobalData::GetGlobalData()->isRunning = TRUE;

	Io::DeleteFileW(L"KrkrzTempWorker.xp3");

	Handle->GetFolder(BasePack, countof(BasePack));

	LocalKrkrPacker.Init();
	

	ULONG Attr = GetFileAttributesW(BasePack);
	if (!(Attr & FILE_ATTRIBUTE_DIRECTORY))
	{
		MessageBoxW(Handle->MainWindow, L"Couldn't regard the target path as a directory", L"KrkrExtract", MB_OK);
		GlobalData::GetGlobalData()->isRunning = FALSE;
		LocalKrkrPacker.InternalReset();
		return INVALID_HANDLE_VALUE;
	}

	Status = Nt_CreateThread(PackerThread, NULL, FALSE, NtCurrentProcess(), &LocalKrkrPacker.hThread);
	if (NT_FAILED(Status))
	{
		MessageBoxW(Handle->MainWindow, L"Cannot launch packer thread", L"KrkrExtract", MB_OK);
		GlobalData::GetGlobalData()->isRunning = FALSE;
		LocalKrkrPacker.InternalReset();
		return INVALID_HANDLE_VALUE;
	}
	return LocalKrkrPacker.hThread;
}

