#include "stdafx.h"

#include "csgo_Audio.h"

#include "RenderView.h"
#include "SourceInterfaces.h"
#include "addresses.h"
#include "MirvWav.h"

#include <shared/detours.h>

#include <string>
#include <mutex>
#include <map>
#include <sstream>

typedef void(__stdcall * CAudioXAudio2_UnkSupplyAudio_t)(DWORD * this_ptr, int numChannels, float * audioData);
typedef void(__cdecl * csgo_MIX_PaintChannels_t)(int paintCountTarget, int unknown);

CAudioXAudio2_UnkSupplyAudio_t detoured_CAudioXAudio2_UnkSupplyAudio;

csgo_MIX_PaintChannels_t detoured_csgo_MIX_PaintChannels;

std::mutex g_csgo_Audio_Mutex;
double g_csgo_Audio_TimeDue = 0;
double g_csgo_Audio_Remainder = 0;
bool g_CAudioXAudio2_RecordAudio_Active = false;
bool g_CAudioXAudio2_FirstCallInLoop = true;
std::wstring g_CAudioXAudio2_RecordAudio_Dir;
std::map<DWORD *, CMirvWav> g_CAudioXAudio2_RecordAudio_Files;

std::vector<WORD> g_CAudioXAudio2_ChannelData;

void __stdcall touring_CAudioXAudio2_UnkSupplyAudio(DWORD * this_ptr, int numChannels, float * audioData)
{
	if (g_CAudioXAudio2_RecordAudio_Active)
	{
		//Tier0_Msg("Calling CAudioXAudio2_UnkSupplyAudio.\n");

		std::map<DWORD *, CMirvWav>::iterator it = g_CAudioXAudio2_RecordAudio_Files.find(this_ptr);

		if (it == g_CAudioXAudio2_RecordAudio_Files.end())
		{
			std::wostringstream os;
			os << g_CAudioXAudio2_RecordAudio_Dir << L"\\audio_" << this_ptr << L".wav";
			std::wstring fileName = os.str();

			it = g_CAudioXAudio2_RecordAudio_Files.emplace(std::piecewise_construct, std::forward_as_tuple(this_ptr), std::forward_as_tuple(fileName.c_str(), numChannels, 44100)).first;
		}

		const int samples = 512;

		if ((int)g_CAudioXAudio2_ChannelData.size() < numChannels)
			g_CAudioXAudio2_ChannelData.resize(numChannels);

		for (int j = 0; j < samples; ++j)
		{
			for (int i = 0; i < numChannels; ++i)
			{
				float fVal = audioData[i*samples + j];
				fVal = min(max(fVal, -32768), 32767);
				fVal = std::round(fVal);
				int iVal = (int)fVal;
				WORD wVal = (WORD)iVal;
				g_CAudioXAudio2_ChannelData[i] = wVal;
			}

			it->second.Append(numChannels, &g_CAudioXAudio2_ChannelData[0]);
		}

	}

	// sorry, but we can't forward mutliple calls in a loop, that will overrun buffers!
	if(g_CAudioXAudio2_FirstCallInLoop)
		detoured_CAudioXAudio2_UnkSupplyAudio(this_ptr, numChannels, audioData);
}


void __cdecl touring_csgo_MIX_PaintChannels(int endtime, int unknown)
{
	std::unique_lock<std::mutex> lock(g_csgo_Audio_Mutex);

	if (!g_CAudioXAudio2_RecordAudio_Active)
	{
		detoured_csgo_MIX_PaintChannels(endtime, unknown);
		return;
	}

	if (g_csgo_Audio_TimeDue <= 0)
		return;

	double fDeltaTime = g_csgo_Audio_TimeDue*44100.0 + g_csgo_Audio_Remainder;
	g_csgo_Audio_TimeDue = 0;
	int deltaTime = (int)(fDeltaTime);
	deltaTime = deltaTime - (deltaTime % 512);
	g_csgo_Audio_Remainder = fDeltaTime - deltaTime;

	while (0 < deltaTime)
	{
		detoured_csgo_MIX_PaintChannels(endtime, unknown);
		deltaTime -= 512;
		endtime += 512;

		g_CAudioXAudio2_FirstCallInLoop = false;
	}

	g_CAudioXAudio2_FirstCallInLoop = true;
}


bool csgo_Audio_Install(void)
{
	static bool firstResult = true;
	static bool firstRun = true;
	if (!firstRun) return firstResult;
	firstRun = false;

	if (AFXADDR_GET(csgo_CAudioXAudio2_vtable)
		&& AFXADDR_GET(csgo_MIX_PaintChannels))
	{
		detoured_csgo_MIX_PaintChannels = (csgo_MIX_PaintChannels_t)DetourApply((BYTE*)AFXADDR_GET(csgo_MIX_PaintChannels), (BYTE *)touring_csgo_MIX_PaintChannels, (int)AFXADDR_GET(csgo_MIX_PaintChannels_DSZ));

		int * vtable = (int*)AFXADDR_GET(csgo_CAudioXAudio2_vtable);

		DetourIfacePtr((DWORD *)&(vtable[1]), touring_CAudioXAudio2_UnkSupplyAudio, (DetourIfacePtr_fn &)detoured_CAudioXAudio2_UnkSupplyAudio);
	}
	else
		firstResult = false;

	return firstResult;
}

bool csgo_Audio_StartRecording(const wchar_t * ansiTakeDir)
{
	if (!csgo_Audio_Install())
		return false;

	csgo_Audio_EndRecording();

	std::unique_lock<std::mutex> lock(g_csgo_Audio_Mutex);

	g_CAudioXAudio2_RecordAudio_Dir = ansiTakeDir;
	g_CAudioXAudio2_RecordAudio_Active = true;
	g_csgo_Audio_TimeDue = 0;
	g_csgo_Audio_Remainder = 0;

	return true;
}

void csgo_Audio_EndRecording(void)
{
	std::unique_lock<std::mutex> lock(g_csgo_Audio_Mutex);

	if (!g_CAudioXAudio2_RecordAudio_Active)
		return;

	g_CAudioXAudio2_RecordAudio_Files.clear();

	g_CAudioXAudio2_RecordAudio_Active = false;
}

void csgo_Audio_FRAME_RENDEREND(void)
{
	std::unique_lock<std::mutex> lock(g_csgo_Audio_Mutex);

	WrpGlobals * glob = g_Hook_VClient_RenderView.GetGlobals();

	if (!glob)
		return;

	g_csgo_Audio_TimeDue += glob->absoluteframetime_get();
}
