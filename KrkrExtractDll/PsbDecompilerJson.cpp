#include "my.h"
#include <Psapi.h>
#include <map>
#include <string>
#include <Shlobj.h>
#include "json/json.h"
#include "PsbFile.h"
#include "lz4.h"
#include "lz4frame.h"
#include "zlib.h"

#pragma comment(lib, "Shell32.lib")
#pragma comment(lib, "jsoncpp.lib")

using std::string;
using std::vector;
using std::map;

#ifndef LZ4_MAGIC
#define LZ4_MAGIC 0x184D2204
#endif

Void TraversalOffsetsTree(PsbJsonExporter& _Psb, map<ULONG, string>& ResNode, PsbCollection *Offsets, string EntryName, Json::Value &Root);

Void TraversalObjectTree(PsbJsonExporter& _Psb, map<ULONG, string>& ResNode, PsbObject *Objects, string ObjectName, Json::Value &Root)
{
	PsbValue *Value = NULL;

	for (ULONG i = 0; i < Objects->Size(); i++)
	{
		auto EntryName = Objects->GetName(i);
		auto EntryBuff = Objects->GetData(i);

		_Psb.Unpack(Value, EntryBuff);
		if (Value == NULL)
			continue;

		switch (Value->GetNodeType())
		{
			case TYPE_COLLECTION:
			{
				Json::Value Node(Json::arrayValue);
				TraversalOffsetsTree(_Psb, ResNode, (PsbCollection*)Value, EntryName, Node);
				Root[EntryName] = Node;
			}
			break;


			case TYPE_OBJECTS:
			{
				Json::Value Node(Json::objectValue);
				TraversalObjectTree(_Psb, ResNode, (PsbObject *)Value, EntryName, Node);
				Root[EntryName] = Node;
			}
			break;

			case TYPE_TRUE:
			case TYPE_FALSE:
			{

				Json::Value Node(Json::booleanValue);
				PsbBool* BoolValue = (PsbBool*)Value;
				Node = BoolValue->GetBoolean();
				Root[EntryName] = Node;
			}
			break;

			case TYPE_STRING_N1:
			{
				Json::Value Node(Json::stringValue);
				PsbString* StringValue = (PsbString*)Value;
				Node = StringValue->GetString();
				Root[EntryName] = Node;
			}
			break;

			case TYPE_NUMBER_N0:
			case TYPE_NUMBER_N1:
			case TYPE_NUMBER_N2:
			case TYPE_NUMBER_N3:
			case TYPE_NUMBER_N4:
			{
				Json::Value Node(Json::intValue);
				PsbNumber* Number = (PsbNumber*)Value;
				Node = Number->GetInteger();
				Root[EntryName] = Node;
			}
			break;

			case TYPE_FLOAT0:
			case TYPE_FLOAT:
			{

				Json::Value Node(Json::realValue);
				PsbNumber* Number = (PsbNumber*)Value;
				Node = Number->GetFloat();
				Root[EntryName] = Node;
			}
			break;

			case TYPE_DOUBLE:
			{
				Json::Value Node(Json::realValue);
				PsbNumber* Number = (PsbNumber*)Value;
				Node = Number->GetDouble();
				Root[EntryName] = Node;
			}
			break;

		case TYPE_NULL:
		{
			Json::Value Node(Json::nullValue);
			Root[EntryName] = Node;
		}
		break;

		case TYPE_NUMBER_N5:
		case TYPE_NUMBER_N6:
		case TYPE_NUMBER_N7:
		case TYPE_NUMBER_N8:
		{
			Json::Value Node(Json::intValue);
			PsbNumber *Number = (PsbNumber*)Value;
			Node = Number->GetInteger();
			Root[EntryName] = Node;
		}
		break;


		case TYPE_RESOURCE_N1:
		case TYPE_RESOURCE_N2:
		case TYPE_RESOURCE_N3:
		case TYPE_RESOURCE_N4:
		{
			CHAR IndexName[32];
			PsbResource* Resource = (PsbResource*)Value;

			Json::Value Node(Json::stringValue);
			
			FormatStringA(IndexName, "%d", Resource->GetIndex());
			ResNode[Resource->GetIndex()] = EntryName;
			Node = "__CompilerBinary__(" + (string)IndexName + ", '" + EntryName + "')";
			Root[EntryName] = Node;
		}
		break;

		default:
			throw std::exception("PsbJson::TraversalObjectTree : unknown node type");
			break;
		}
	}
}

Void TraversalOffsetsTree(PsbJsonExporter& _Psb, map<ULONG, string>& ResNode, PsbCollection *Offsets, string EntryName, Json::Value &Root) 
{
	PsbValue *Value = NULL;

	for (ULONG i = 0; i < Offsets->Size(); i++) 
	{
		auto EntryBuff = Offsets->Get(i);
		_Psb.Unpack(Value, EntryBuff);

		if (Value == NULL)
			continue;

		switch (Value->GetNodeType())
		{
			case TYPE_COLLECTION:
			{
				Json::Value Node(Json::arrayValue);
				TraversalOffsetsTree(_Psb, ResNode, (PsbCollection*)Value, EntryName, Node);
				Root.append(Node);
			}
			break;

			case TYPE_OBJECTS:
			{
				Json::Value Node(Json::objectValue);
				TraversalObjectTree(_Psb, ResNode, (PsbObject*)Value, EntryName, Node);
				Root.append(Node);
			}
			break;

			case TYPE_TRUE:
			case TYPE_FALSE:
			{
				Json::Value Node(Json::booleanValue);
				PsbBool* BoolValue = (PsbBool*)Value;
				Node = BoolValue->GetBoolean();
				Root.append(Node);
			}
			break;

			case TYPE_STRING_N1:
			{
				Json::Value Node(Json::stringValue);
				PsbString* StringValue = (PsbString*)Value;
				Node = StringValue->GetString();
				Root.append(Node);
			}
			break;

			case TYPE_NUMBER_N0:
			case TYPE_NUMBER_N1:
			case TYPE_NUMBER_N2:
			case TYPE_NUMBER_N3:
			case TYPE_NUMBER_N4:
			{
				Json::Value Node(Json::intValue);
				PsbNumber* Number = (PsbNumber*)Value;
				Node = Number->GetInteger();
				Root.append(Node);
			}
			break;

			case TYPE_FLOAT0:
			case TYPE_FLOAT:
			{
				Json::Value Node(Json::realValue);
				PsbNumber* Number = (PsbNumber*)Value;
				Node = Number->GetFloat();
				Root.append(Node);
			}
			break;

			case TYPE_DOUBLE:
			{
				Json::Value Node(Json::realValue);
				PsbNumber* Number = (PsbNumber*)Value;
				Node = Number->GetDouble();
				Root.append(Node);
			}
			break;

			case TYPE_NULL:
			{
				Json::Value Node(Json::nullValue);
				Root.append(Node);
			}
			break;

			case TYPE_NUMBER_N5:
			case TYPE_NUMBER_N6:
			case TYPE_NUMBER_N7:
			case TYPE_NUMBER_N8:
			{
				Json::Value Node(Json::intValue);
				PsbNumber* Number = (PsbNumber*)Value;
				Node = Number->GetInteger();
				Root.append(Node);
			}
			break;

			case TYPE_RESOURCE_N1:
			case TYPE_RESOURCE_N2:
			case TYPE_RESOURCE_N3:
			case TYPE_RESOURCE_N4:
			{
				CHAR IndexName[32];
				PsbResource* Resource = (PsbResource*)Value;

				Json::Value Node(Json::stringValue);

				FormatStringA(IndexName, "%d", Resource->GetIndex());
				ResNode[Resource->GetIndex()] = EntryName;
				Node = "__CompilerBinary__(" + (string)IndexName + ", '" + EntryName + "')";

				Root.append(Node);
			}
				break;

			default:
				throw std::exception("PsbJson::TraversalOffsetsTree : unknown node type");
				break;
		}
	}
}


Void ExportResource(PsbJsonExporter& _Psb, map<ULONG, string>& ResNode, LPCWSTR FileName, LPCWSTR DirName)
{
	NTSTATUS Status;

	std::wstring ResDirName = FileName;
	ResDirName += L"\\";
	ResDirName += FileName;
	ResDirName += L".dir";

	if (GetFileAttributesW(ResDirName.c_str()) == 0xFFFFFFFF)
		SHCreateDirectory(NULL, ResDirName.c_str());


	for (ULONG i = 0; i < _Psb.ChunkOffsets->Size(); i++)
	{
		WCHAR BinaryName[MAX_PATH];
		WCHAR OutputName[MAX_PATH];

		RtlZeroMemory(BinaryName, sizeof(BinaryName));
		MultiByteToWideChar(CP_UTF8, 0, ResNode[i].c_str(), ResNode[i].length(), BinaryName, countof(BinaryName) - 1);

		ULONG Offset = _Psb.ChunkOffsets->Get(i);
		ULONG Length = _Psb.ChunkLengths->Get(i);

		FormatStringW(OutputName, L"%s\\%s", ResDirName.c_str(), BinaryName);

		NtFileDisk File;
		Status = File.Create(OutputName);
		if (NT_FAILED(Status))
			continue;

		File.Write((PVOID)(_Psb.ChunkData + Offset), Length);
		File.Close();
	}

}


NTSTATUS NTAPI FindEmoteKeyByMark(PULONG PrivateKey)
{
	HMODULE      hModule;
	NTSTATUS     Status;
	ULONG64      Magic[4];
	MODULEINFO   ModudleInfo;
	ULONG_PTR    iPos;
	ULONG        AddressOfPrivate;

	LOOP_ONCE
	{
		Status = STATUS_UNSUCCESSFUL;

		hModule = GetModuleHandleW(L"emotedriver.dll");

		if (!hModule)
			hModule = LoadLibraryW(L"emotedriver.dll");

		if (!hModule)
			hModule = LoadLibraryW(L"plugin\\emotedriver.dll");

		if (!hModule)
			break;

		GetModuleInformation(GetCurrentProcess(), hModule, &ModudleInfo, NULL);

		iPos = 0;
		LOOP_FOREVER
		{
			if (iPos >= ModudleInfo.SizeOfImage)
			break;

			Magic[0] = *(PULONG64)((ULONG_PTR)hModule + iPos + 0);
			Magic[1] = *(PULONG64)((ULONG_PTR)hModule + iPos + 8);
			Magic[2] = *(PULONG64)((ULONG_PTR)hModule + iPos + 16);
			Magic[3] = *(PULONG64)((ULONG_PTR)hModule + iPos + 24);


			if (Magic[0] == 0x00004E414E512331 &&
				Magic[1] == 0x000000464E492331 &&
				Magic[2] == 0x000000444E492331 &&
				Magic[3] == 0x00004E414E532331
				)
			{
				iPos += 0x20;
				AddressOfPrivate = *(PULONG)((ULONG_PTR)hModule + iPos);

				*PrivateKey = StringToInt32A((LPCSTR)AddressOfPrivate);
				Status = STATUS_SUCCESS;

				break;
			}
			else
			{
				iPos++;
			}
		}
	}

	return Status;
}

NTSTATUS NTAPI FindEmoteKeyByParse(PULONG PrivateKey)
{
	HMODULE    hModule;
	PVOID      FilterTextureAddress;
	ULONG64    Magic[2];
	ULONG_PTR  iPos;
	ULONG      AddressOfPrivate;
	NTSTATUS   Status;

	if (PrivateKey)
		*PrivateKey = 0;

	Status = STATUS_UNSUCCESSFUL;

	LOOP_ONCE
	{
		hModule = GetModuleHandleW(L"emotedriver.dll");

		if (!hModule)
			hModule = LoadLibraryW(L"emotedriver.dll");

		if (!hModule)
			hModule = LoadLibraryW(L"plugin\\emotedriver.dll");

		if (!hModule)
			return STATUS_NO_SUCH_FILE;

		FilterTextureAddress = GetProcAddress(hModule, "?EmoteFilterTexture@@YAXPAEKP6AX0K@Z@Z");

		if (!FilterTextureAddress)
			return STATUS_NOT_SUPPORTED;


		iPos = 0;

		LOOP_FOREVER
		{
			if (iPos >= 500)
			break;

			Magic[0] = *(PULONG64)((ULONG_PTR)FilterTextureAddress + iPos + 0);
			Magic[1] = *(PULONG64)((ULONG_PTR)FilterTextureAddress + iPos + 8);

			if (Magic[0] == 0x458B00000000A364 && Magic[1] == 0x68085D8B104D8B0C)
			{
				//push Offset
				iPos += 0x10;
				AddressOfPrivate = *(PULONG)((ULONG_PTR)FilterTextureAddress + iPos);

				*PrivateKey = StringToInt32A((LPCSTR)AddressOfPrivate);
				Status = STATUS_SUCCESS;

				break;
			}
			else
			{
				iPos++;
			}
		}
	}
	return Status;
}
NTSTATUS NTAPI FindEmoteKeyByEmotePlayer(PULONG PrivateKey)
{
	ULONG64    Magic[3];
	PVOID      ModuleAddress;
	ULONG_PTR  iPos, SizeOfImage;
	ULONG      AddressOfPrivate;
	MODULEINFO ModuleInfo;
	NTSTATUS   Status;

	if (PrivateKey)
		*PrivateKey = 0;

	Status = STATUS_UNSUCCESSFUL;


	LOOP_ONCE
	{
		ModuleAddress = Nt_GetModuleHandle(L"emoteplayer.dll");

		if (!ModuleAddress)
			ModuleAddress = Nt_LoadLibrary(L"emoteplayer.dll");

		if (!ModuleAddress)
			ModuleAddress = Nt_LoadLibrary(L"plugin\\emoteplayer.dll");

		if (!ModuleAddress)
			return STATUS_NO_SUCH_FILE;

		RtlZeroMemory(&ModuleInfo, sizeof(ModuleInfo));
		GetModuleInformation(GetCurrentProcess(), (HMODULE)ModuleAddress, &ModuleInfo, sizeof(ModuleInfo));
		iPos = 0;
		SizeOfImage = ModuleInfo.SizeOfImage;

		LOOP_FOREVER
		{
			if (iPos >= SizeOfImage)
			break;

			Magic[0] = *(PULONG64)((ULONG_PTR)ModuleAddress + iPos + 0);
			Magic[1] = *(PULONG64)((ULONG_PTR)ModuleAddress + iPos + 8);

			if (Magic[0] == 0xCD15FFFFFF3885C7 && Magic[1] == 0xFFFFFF3C85C7075B)
			{
				//push Offset
				/*
				CPU Disasm
				Address                      Hex dump                       Command                                      Comments
				05B37A0F                     |> \68 5C9AC005                push offset 05C09A5C                         ; ASCII "149203383"
				05B37A14                     |.  89B5 78FFFFFF              mov dword ptr [ebp-88],esi
				05B37A1A                     |.  E8 4D650800                call 05BBDF6C

				*/


				iPos -= (10 + 5 + 6 + 4);

				//push
				if (((PBYTE)ModuleAddress)[iPos - 1] == 0x68)
				{
					AddressOfPrivate = *(PULONG)((ULONG_PTR)ModuleAddress + iPos);

					*PrivateKey = StringToInt32A((LPCSTR)AddressOfPrivate);
					Status = STATUS_SUCCESS;

					break;
				}
				else
				{
					iPos += (10 + 5 + 6 + 4) + sizeof(Magic[0]) * 2;
				}
			}
			else
			{
				iPos++;
			}
		}
	}
	return Status;
}

static DWORD EmotePrivateKey = 0;

ULONG GetPrivateKey()
{
	ULONG    Key;

	if (EmotePrivateKey)
		return EmotePrivateKey;

	if (FindEmoteKeyByParse(&Key) >= 0 || FindEmoteKeyByMark(&Key) >= 0 || FindEmoteKeyByEmotePlayer(&Key) >= 0)
		EmotePrivateKey = Key;
	else
		Key = 0;
	
	return Key;
}


static DWORD m_key[6] = {0};

void EmoteDecrypt(LPBYTE data, int offset, int length)
{
	for (int i = 0; i < length; ++i)
	{
		if (0 == m_key[4])
		{
			DWORD v5 = m_key[3];
			DWORD v6 = m_key[0] ^ (m_key[0] << 11);
			m_key[0] = m_key[1];
			m_key[1] = m_key[2];
			DWORD eax = v6 ^ v5 ^ ((v6 ^ (v5 >> 11)) >> 8);
			m_key[2] = v5;
			m_key[3] = eax;
			m_key[4] = eax;
		}
		data[offset + i] ^= (BYTE)m_key[4];
		m_key[4] >>= 8;
	}
}

static unsigned char *getDataFromMDF(const unsigned char *buff, unsigned long &size)
{
	if (size <= 10 || *(PDWORD)buff == TAG3("mdf")) return NULL;

	ULONG Uncompsize = *(unsigned long*)&buff[4];
	PBYTE uncomp = (PBYTE)AllocateMemoryP(Uncompsize);
	if (Z_OK != uncompress(uncomp, &Uncompsize, buff + 8, size - 8))
	{
		FreeMemoryP(uncomp);
		return NULL;
	}
	size = Uncompsize;
	return uncomp;
}


static unsigned char *getDataFromLz4(PBYTE buff, ULONG &size)
{
	if (size <= 10 || *(PDWORD)buff != LZ4_MAGIC) 
		return NULL;

	ULONG uncompsize = *(unsigned long*)&buff[4];

	LZ4F_decompressionContext_t context;
	LZ4F_frameInfo_t frameInfo;
	LZ4F_errorCode_t err;
	size_t srcSize = size;
	unsigned char *uncompr_data;
	size_t pos, dstPos;
	size_t dstSize;

	err = LZ4F_createDecompressionContext(&context, LZ4F_VERSION);
	if (LZ4F_isError(err))
	{
		return NULL;
	}

	err = LZ4F_getFrameInfo(context, &frameInfo, buff, &srcSize);
	if (LZ4F_isError(err))
		return NULL;

	pos = srcSize;
	dstPos = 0;
	uncompr_data = (PBYTE)AllocateMemoryP(LODWORD(frameInfo.contentSize));

	do
	{
		dstSize = LODWORD(frameInfo.contentSize) - dstPos;
		srcSize = size - pos;

		err = LZ4F_decompress(context, &uncompr_data[dstPos], &dstSize, &buff[pos], &srcSize, NULL);

		if (LZ4F_isError(err))
		{
			FreeMemoryP(uncompr_data);
			return nullptr;
		}

		dstPos += dstSize;
		pos += srcSize;
	} while (err);

	size = LODWORD(frameInfo.contentSize);
	return uncompr_data;
}

NTSTATUS WINAPI DecompilePsbJson(IStream* PsbStream, LPCWSTR BasePath, LPCWSTR FileName)
{
	NTSTATUS           Status;
	Json::Value        Root;
	NtFileDisk         File;
	std::wstring       JsonFileName;
	map<ULONG, string> ResNode;
	ULONG              Length;
	PBYTE              Buffer;
	LARGE_INTEGER      Offset;
	STATSTG            Stat;


	JsonFileName = BasePath;
	JsonFileName += L"\\";
	JsonFileName += FileName;
	JsonFileName += L".json";

	Offset.QuadPart = 0;
	PsbStream->Seek(Offset, FILE_BEGIN, NULL);
	PsbStream->Stat(&Stat, STATFLAG_DEFAULT);

	Length = Stat.cbSize.LowPart;
	Buffer = (PBYTE)AllocateMemoryP(Length);
	if (!Buffer)
		return STATUS_NO_MEMORY;

	PsbStream->Read(Buffer, Length, NULL);

	
	Status = File.Create(JsonFileName.c_str());
	if (NT_FAILED(Status))
		return Status;

	if (Length > 8)
	{
		if (RtlCompareMemory(Buffer, "PSB", 4) != 4)
		{
			LPBYTE uncomp = NULL;

			if (*(PDWORD)(Buffer) == LZ4_MAGIC)
				uncomp = getDataFromLz4(Buffer, Length);
			else
				uncomp = getDataFromMDF(Buffer, Length);

			if (uncomp)
			{
				FreeMemoryP(Buffer);
				Buffer = uncomp;
			}
		}

	}
		
	if (Length < 64 || RtlCompareMemory(Buffer, "PSB", 4) != 4)
		return STATUS_INVALID_BLOCK_LENGTH;

	typedef struct PSB_HEADER_V2
	{
		DWORD  HeaderMagic;
		USHORT Version;
		USHORT Flag;
		LPBYTE pHeader;
		LPBYTE pStringIndex;
		LPBYTE pStringOffsetArray;
		LPCSTR pStringPool;
		LPBYTE pBinOffsetArray;
		LPBYTE pBinSizeArray;
		LPBYTE pBinaryPool;
		LPBYTE pRootCode;
	} PSB_HEADER_V2, *pPSB_HEADER_V2;

	PSB_HEADER_V2* pInfo = (PSB_HEADER_V2*)Buffer;

	if (pInfo->Version != 2 && pInfo->Version != 3)
	{
		FreeMemoryP(Buffer);
		return STATUS_NO_MATCH;
	}
	else
	{
		m_key[0] = 0x075BCD15;
		m_key[1] = 0x159A55E5;
		m_key[2] = 0x1F123BB5;
		m_key[3] = GetPrivateKey();
		m_key[4] = 0;
		m_key[5] = 0;

		if (pInfo->Flag & 1 && pInfo->Version == 3)
			EmoteDecrypt(Buffer + 0x8, 0, pInfo->Version == 3 ? 0x24 : 0x20);

		//PSB V2 will not mark the field
		//root is a directory?
		if (pInfo->Flag & 2 ||
			((ULONG_PTR)pInfo->pRootCode >= Length) ||
			Buffer[(ULONG_PTR)pInfo->pRootCode] != 0x21)
		{
			EmoteDecrypt(Buffer + (ULONG_PTR)pInfo->pStringIndex, 0,
				(ULONG_PTR)pInfo->pBinOffsetArray - (ULONG_PTR)pInfo->pStringIndex);

			if (Buffer[(ULONG_PTR)pInfo->pRootCode] != 0x21)
			{
				FreeMemoryP(Buffer);
				return STATUS_UNSUCCESSFUL;
			}
		}
	}

	PsbJsonExporter Psb(Buffer);
	auto Objects = Psb.GetObject();
	TraversalObjectTree(Psb, ResNode, Objects, "", Root);

	string Output = Root.toStyledString();
	File.Write((PVoid)Output.c_str(), Output.length());
	File.Close();

	ExportResource(Psb, ResNode, FileName, BasePath);
	FreeMemoryP(Buffer);
	return Status;
}
