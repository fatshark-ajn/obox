#include "kernel.h"
#include <circle/timer.h>
#include <circle/string.h>
#include <circle/memory.h>
#include <circle/synchronize.h>
#include <fatfs/ff.h>

FIL log_file = {0};
bool running = TRUE;
int last_asset_len = 0;

CBcmFrameBuffer *CKernel::s_pFrameBuffer(0);

struct fb_definition {
	void *ptr;
	int width;
	int height;
	int pitch;
};

struct fb_definition framebuffer = {0};
TGamePadState gp_states[MAX_GAMEPADS] = {0};

extern "C" {
	void odin_startup_runtime(void);
	void game_update(int64_t);
	void game_render(void);
	void game_shutdown(void);
}

extern "C" {
	void kernel_write_log(const char *str) {
		f_write(&log_file, str, CString(str).GetLength(), 0);
		f_sync(&log_file);
	}

	void kernel_halt() {
		running = FALSE;
	}

	void *kernel_alloc(size_t size) {
		return CMemorySystem::HeapAllocate(size, HEAP_DEFAULT_NEW);	
	}

	void kernel_dealloc(void *ptr) {
		CMemorySystem::HeapFree(ptr);
	}

	void kernel_sleep_ms(int64_t ms) {
		CTimer::SimpleMsDelay(ms);
	}

	struct fb_definition *kernel_fb_definition(void) {
		return &framebuffer;
	}

	void kernel_wait_for_vsync(void) {
		CKernel::s_pFrameBuffer->WaitForVerticalSync();	
	}

	unsigned kernel_read_pad(int index) {
		unsigned buttons = 0;
		if (index < 0 || index >= MAX_GAMEPADS)
			return buttons;

		DisableIRQs();
		DisableFIQs();

		// Just read the digital buttons for now.
		buttons = gp_states[index].buttons;
		
		EnableFIQs();
		EnableIRQs();

		return buttons;
	}

	void *kernel_load_asset(const char *path) {
		FIL fp;
		last_asset_len = 0;

		if (f_open(&fp, path, FA_READ) != FR_OK)
			return 0;

		last_asset_len = (int)f_size(&fp);
		if (last_asset_len < 0) {
			last_asset_len = 0;
			f_close(&fp);
			return 0;
		}

		void *ptr = CMemorySystem::HeapAllocate(last_asset_len, HEAP_DEFAULT_NEW);
		if (f_read(&fp, ptr, last_asset_len, 0) != FR_OK) {
			last_asset_len = 0;
			f_close(&fp);
			return 0;
		}

		f_close(&fp);
		return ptr;
	}
	
	int kernel_load_asset_len() {
		return last_asset_len;
	}
}

CKernel::CKernel (void)
:	m_Timer(&m_Interrupt),
	m_USBHCI(&m_Interrupt, &m_Timer, TRUE),
	m_EMMC(&m_Interrupt, &m_Timer, 0)
{
	s_pFrameBuffer = new CBcmFrameBuffer(m_Options.GetWidth(), m_Options.GetHeight(), 32);
	if (!s_pFrameBuffer->Initialize()) {
		delete s_pFrameBuffer;
		s_pFrameBuffer = 0;
	}

	for (int i = 0; i < MAX_GAMEPADS; i++) {
		m_pGamePad[i] = 0;
	}
}

CKernel::~CKernel (void)
{
	if (s_pFrameBuffer)
		delete s_pFrameBuffer;
}

boolean CKernel::Initialize (void)
{
	boolean bOK = TRUE;
	if (bOK)
		bOK = m_Interrupt.Initialize();

	if (bOK)
		bOK = m_Timer.Initialize();

	if (bOK)
		bOK = m_USBHCI.Initialize();

	if (bOK)
		bOK = m_EMMC.Initialize();

	return bOK;
}

TShutdownMode CKernel::Run (void)
{
	u64 update_ticks;
	u64 render_ticks;
	FATFS emmc_fs;
	
	if (f_mount(&emmc_fs, DRIVE, 1) != FR_OK)
		goto shutdown;

	if (f_open(&log_file, LOGFILE, FA_WRITE|FA_CREATE_ALWAYS) != FR_OK)
		goto shutdown;

	kernel_write_log("Logging initialize!\n");

	if (!s_pFrameBuffer) {
		kernel_write_log("Could not initialize framebuffer!\n");
		goto shutdown;
	} else if (s_pFrameBuffer->GetDepth() != 32) {
		kernel_write_log("Invalid framebuffer format!\n");
		goto shutdown;
	} else {	
		framebuffer.ptr = (void*)(u64)s_pFrameBuffer->GetBuffer();
		framebuffer.pitch = s_pFrameBuffer->GetPitch();
		framebuffer.width = s_pFrameBuffer->GetWidth();
		framebuffer.height = s_pFrameBuffer->GetHeight();
	}

	update_ticks = render_ticks = CTimer::GetClockTicks64();
	odin_startup_runtime();

	while (running)
	{
		for (int i = 0; m_USBHCI.UpdatePlugAndPlay() && i < MAX_GAMEPADS; i++) {
			if (m_pGamePad[i] != 0)
				continue;
			
			if ((m_pGamePad[i] = (CUSBGamePadDevice*)m_DeviceNameService.GetDevice("upad", i + 1, FALSE)) == 0)
				continue;

			const TGamePadState *pState = m_pGamePad[i]->GetInitialState();
			if (!pState) {
				kernel_write_log("Could not initialize gamepad!\n");
				m_pGamePad[i] = 0;
				continue;
			} else {
				kernel_write_log("Gamepad connected!\n");
				gp_states[i] = *pState;
			}

			m_pGamePad[i]->RegisterRemovedHandler(GamePadRemovedHandler, this);
			m_pGamePad[i]->RegisterStatusHandler(GamePadStatusHandler);
		}
	
		u64 now = CTimer::GetClockTicks64();
		int64_t dt = (int64_t)(((double)(now - update_ticks) / (double)CLOCKHZ) * 1000000.0) * 1000;
		if (dt <= 0)
			continue;
		update_ticks = now;
		
		game_update(dt);
		
		// TODO: Lets move this to another CPU core at some point.
		if ((now - render_ticks) >= (CLOCKHZ / 60)) {
			render_ticks = now;
			game_render();
		}
	}
	game_shutdown();

shutdown:

	f_close(&log_file);
	f_unmount(DRIVE);
	
	return ShutdownHalt;
}

// NOTE: These two functions are called by the ISR so DON'T DO ANYTHING here. Just write the data!

void CKernel::GamePadStatusHandler (unsigned nDeviceIndex, const TGamePadState *pState) {
	gp_states[nDeviceIndex] = *pState;
}

void CKernel::GamePadRemovedHandler (CDevice *pDevice, void *pContext) {
	CKernel *pThis = (CKernel*)pContext;
	for (int i = 0; i < MAX_GAMEPADS; i++) {
		if (pThis->m_pGamePad[i] == (CUSBGamePadDevice*)pDevice) {
			pThis->m_pGamePad[i] = 0;
			return;
		}
	}
}
