// PC preview: runs the UI in an SDL2 window with keyboard controls.
//   arrows = D-pad   Z/Enter = A   X/Backspace = B   Q/W = L/R
//   Space = Start    Tab = Select  M = Menu          Esc = quit
#include "emu.h"
#include "sfx.h"
#include "ui.h"

#include <SDL2/SDL.h>

static void audio_cb(void *user, Uint8 *stream, int len)
{
	(void)user;
	sfx_render((int16_t *)stream, len / 2);
	emu_audio_mix((int16_t *)stream, len / 2, 48000);
}

static unsigned read_buttons(void)
{
	const Uint8 *k = SDL_GetKeyboardState(NULL);
	unsigned b = 0;
	if (k[SDL_SCANCODE_UP]) b |= BTN_UP;
	if (k[SDL_SCANCODE_DOWN]) b |= BTN_DOWN;
	if (k[SDL_SCANCODE_LEFT]) b |= BTN_LEFT;
	if (k[SDL_SCANCODE_RIGHT]) b |= BTN_RIGHT;
	if (k[SDL_SCANCODE_Z] || k[SDL_SCANCODE_RETURN]) b |= BTN_A;
	if (k[SDL_SCANCODE_X] || k[SDL_SCANCODE_BACKSPACE]) b |= BTN_B;
	if (k[SDL_SCANCODE_A]) b |= BTN_X;
	if (k[SDL_SCANCODE_S]) b |= BTN_Y;
	if (k[SDL_SCANCODE_Q]) b |= BTN_L;
	if (k[SDL_SCANCODE_W]) b |= BTN_R;
	if (k[SDL_SCANCODE_SPACE]) b |= BTN_START;
	if (k[SDL_SCANCODE_TAB]) b |= BTN_SELECT;
	if (k[SDL_SCANCODE_M]) b |= BTN_MENU;
	return b;
}

int main(void)
{
	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0) {
		SDL_Log("SDL_Init: %s", SDL_GetError());
		return 1;
	}
	SDL_Window *win = SDL_CreateWindow("cozisboy", SDL_WINDOWPOS_CENTERED,
					   SDL_WINDOWPOS_CENTERED, UI_W * 2, UI_H * 2,
					   SDL_WINDOW_RESIZABLE);
	SDL_Renderer *ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_PRESENTVSYNC);
	SDL_RenderSetLogicalSize(ren, UI_W, UI_H);
	SDL_RenderSetIntegerScale(ren, SDL_TRUE);
	SDL_Texture *tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_XRGB8888,
					     SDL_TEXTUREACCESS_STREAMING, UI_W, UI_H);
	static uint32_t buf[UI_W * UI_H];

	ui_init();
	SDL_AudioSpec want = {.freq = 48000, .format = AUDIO_S16SYS, .channels = 1,
			      .samples = 512, .callback = audio_cb}, have;
	SDL_AudioDeviceID dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
	if (dev) {
		sfx_init(have.freq);
		SDL_PauseAudioDevice(dev, 0);
	} else {
		SDL_Log("no audio: %s", SDL_GetError());
	}
	Uint64 last = SDL_GetPerformanceCounter();
	for (;;) {
		SDL_Event e;
		while (SDL_PollEvent(&e))
			if (e.type == SDL_QUIT ||
			    (e.type == SDL_KEYDOWN && e.key.keysym.scancode == SDL_SCANCODE_ESCAPE))
				goto out;
		Uint64 now = SDL_GetPerformanceCounter();
		float dt = (float)(now - last) / (float)SDL_GetPerformanceFrequency();
		last = now;
		if (dt > 0.1f)
			dt = 0.1f;

		ui_frame(buf, UI_W, dt, read_buttons());
		SDL_UpdateTexture(tex, NULL, buf, UI_W * 4);
		SDL_RenderClear(ren);
		SDL_RenderCopy(ren, tex, NULL, NULL);
		SDL_RenderPresent(ren);
	}
out:
	SDL_Quit();
	return 0;
}
