#include "Core.h"
#if defined CC_BUILD_WEB && !defined CC_BUILD_SDL
#include "_WindowBase.h"
#include "Game.h"
#include "String.h"
#include "Funcs.h"
#include "ExtMath.h"
#include "Bitmap.h"
#include "Errors.h"
#include <emscripten/emscripten.h>
#include <emscripten/html5.h>
#include <emscripten/key_codes.h>
extern int interop_CanvasWidth(void); 
extern int interop_CanvasHeight(void);
extern int interop_ScreenWidth(void);
extern int interop_ScreenHeight(void);

static cc_bool keyboardOpen, needResize;
static int RawDpiScale(int x)    { return (int)(x * emscripten_get_device_pixel_ratio()); }
static int GetScreenWidth(void)  { return RawDpiScale(interop_ScreenWidth()); }
static int GetScreenHeight(void) { return RawDpiScale(interop_ScreenHeight()); }

static void UpdateWindowBounds(void) {
	int width  = interop_CanvasWidth();
	int height = interop_CanvasHeight();
	if (width == WindowInfo.Width && height == WindowInfo.Height) return;

	WindowInfo.Width  = width;
	WindowInfo.Height = height;
	Event_RaiseVoid(&WindowEvents.Resized);
}

static void SetFullscreenBounds(void) {
	int width  = GetScreenWidth();
	int height = GetScreenHeight();
	emscripten_set_canvas_element_size("#canvas", width, height);
}

/* Browser only allows pointer lock requests in response to user input */
static void DeferredEnableRawMouse(void) {
	EmscriptenPointerlockChangeEvent status;
	if (!Input_RawMode) return;

	status.isActive = false;
	emscripten_get_pointerlock_status(&status);
	if (!status.isActive) emscripten_request_pointerlock("#canvas", false);
}

static EM_BOOL OnMouseWheel(int type, const EmscriptenWheelEvent* ev, void* data) {
	/* TODO: The scale factor isn't standardised.. is there a better way though? */
	Mouse_ScrollWheel(-Math_Sign(ev->deltaY));
	DeferredEnableRawMouse();
	return true;
}

static EM_BOOL OnMouseButton(int type, const EmscriptenMouseEvent* ev, void* data) {
	cc_bool down = type == EMSCRIPTEN_EVENT_MOUSEDOWN;
	/* https://stackoverflow.com/questions/60895686/how-to-get-mouse-buttons-4-5-browser-back-browser-forward-working-in-firef */
	switch (ev->button) {
		case 0: Input_Set(KEY_LMOUSE, down); break;
		case 1: Input_Set(KEY_MMOUSE, down); break;
		case 2: Input_Set(KEY_RMOUSE, down); break;
		case 3: Input_Set(KEY_XBUTTON1, down); break;
		case 4: Input_Set(KEY_XBUTTON2, down); break;
	}

	DeferredEnableRawMouse();
	return true;
}
 
/* input coordinates are CSS pixels, remap to internal pixels */
static void RescaleXY(int* x, int* y) {
	double css_width, css_height;
	emscripten_get_element_css_size("#canvas", &css_width, &css_height);

	if (css_width && css_height) {
		*x = (int)(*x * WindowInfo.Width  / css_width );
		*y = (int)(*y * WindowInfo.Height / css_height);
	} else {
		/* If css width or height is 0, something is bogus    */
		/* Better to avoid divsision by 0 in that case though */
	}
}

static EM_BOOL OnMouseMove(int type, const EmscriptenMouseEvent* ev, void* data) {
	int x, y, buttons = ev->buttons;
	/* Set before position change, in case mouse buttons changed when outside window */
	Input_SetNonRepeatable(KEY_LMOUSE, buttons & 0x01);
	Input_SetNonRepeatable(KEY_RMOUSE, buttons & 0x02);
	Input_SetNonRepeatable(KEY_MMOUSE, buttons & 0x04);

	x = ev->targetX; y = ev->targetY;
	RescaleXY(&x, &y);
	Pointer_SetPosition(0, x, y);
	if (Input_RawMode) Event_RaiseRawMove(&PointerEvents.RawMoved, ev->movementX, ev->movementY);
	return true;
}

/* TODO: Also query mouse coordinates globally (in OnMouseMove) and reuse interop_AdjustXY here */
extern void interop_AdjustXY(int* x, int* y);

static EM_BOOL OnTouchStart(int type, const EmscriptenTouchEvent* ev, void* data) {
	const EmscriptenTouchPoint* t;
	int i, x, y;
	for (i = 0; i < ev->numTouches; ++i) {
		t = &ev->touches[i];
		if (!t->isChanged) continue;
		x = t->targetX; y = t->targetY;

		interop_AdjustXY(&x, &y);
		RescaleXY(&x, &y);
		Input_AddTouch(t->identifier, x, y);
	}
	/* Return true, as otherwise touch event is converted into a mouse event */
	return true;
}

static EM_BOOL OnTouchMove(int type, const EmscriptenTouchEvent* ev, void* data) {
	const EmscriptenTouchPoint* t;
	int i, x, y;
	for (i = 0; i < ev->numTouches; ++i) {
		t = &ev->touches[i];
		if (!t->isChanged) continue;
		x = t->targetX; y = t->targetY;

		interop_AdjustXY(&x, &y);
		RescaleXY(&x, &y);
		Input_UpdateTouch(t->identifier, x, y);
	}
	/* Return true, as otherwise touch event is converted into a mouse event */
	return true;
}

static EM_BOOL OnTouchEnd(int type, const EmscriptenTouchEvent* ev, void* data) {
	const EmscriptenTouchPoint* t;
	int i, x, y;
	for (i = 0; i < ev->numTouches; ++i) {
		t = &ev->touches[i];
		if (!t->isChanged) continue;
		x = t->targetX; y = t->targetY;

		interop_AdjustXY(&x, &y);
		RescaleXY(&x, &y);
		Input_RemoveTouch(t->identifier, x, y);
	}
	/* Don't intercept touchend events while keyboard is open, that way */
	/* user can still touch to move the caret position in input textbox. */
	return !keyboardOpen;
}

static EM_BOOL OnFocus(int type, const EmscriptenFocusEvent* ev, void* data) {
	WindowInfo.Focused = type == EMSCRIPTEN_EVENT_FOCUS;
	Event_RaiseVoid(&WindowEvents.FocusChanged);
	return true;
}

static EM_BOOL OnResize(int type, const EmscriptenUiEvent* ev, void* data) {
	UpdateWindowBounds(); needResize = true;
	return true;
}
/* This is only raised when going into fullscreen */
static EM_BOOL OnCanvasResize(int type, const void* reserved, void* data) {
	UpdateWindowBounds(); needResize = true;
	return false;
}
static EM_BOOL OnFullscreenChange(int type, const EmscriptenFullscreenChangeEvent* ev, void* data) {
	UpdateWindowBounds(); needResize = true;
	return false;
}

static const char* OnBeforeUnload(int type, const void* ev, void *data) {
	if (!Game_ShouldClose()) {
		/* Exit pointer lock, otherwise when you press Ctrl+W, the */
		/*  cursor remains invisible in the confirmation dialog */
		emscripten_exit_pointerlock();
		return "You have unsaved changes. Are you sure you want to quit?";
	}
	Window_Close();
	return NULL;
}

static EM_BOOL OnVisibilityChanged(int eventType, const EmscriptenVisibilityChangeEvent* ev, void* data) {
	cc_bool inactive = ev->visibilityState == EMSCRIPTEN_VISIBILITY_HIDDEN;
	if (WindowInfo.Inactive == inactive) return false;

	WindowInfo.Inactive = inactive;
	Event_RaiseVoid(&WindowEvents.InactiveChanged);
	return false;
}

static int MapNativeKey(int k, int l) {
	if (k >= '0' && k <= '9') return k;
	if (k >= 'A' && k <= 'Z') return k;
	if (k >= DOM_VK_F1      && k <= DOM_VK_F24)     { return KEY_F1  + (k - DOM_VK_F1); }
	if (k >= DOM_VK_NUMPAD0 && k <= DOM_VK_NUMPAD9) { return KEY_KP0 + (k - DOM_VK_NUMPAD0); }

	switch (k) {
	case DOM_VK_BACK_SPACE: return KEY_BACKSPACE;
	case DOM_VK_TAB:        return KEY_TAB;
	case DOM_VK_RETURN:     return l == DOM_KEY_LOCATION_NUMPAD ? KEY_KP_ENTER : KEY_ENTER;
	case DOM_VK_SHIFT:      return l == DOM_KEY_LOCATION_RIGHT  ? KEY_RSHIFT : KEY_LSHIFT;
	case DOM_VK_CONTROL:    return l == DOM_KEY_LOCATION_RIGHT  ? KEY_RCTRL  : KEY_LCTRL;
	case DOM_VK_ALT:        return l == DOM_KEY_LOCATION_RIGHT  ? KEY_RALT   : KEY_LALT;
	case DOM_VK_PAUSE:      return KEY_PAUSE;
	case DOM_VK_CAPS_LOCK:  return KEY_CAPSLOCK;
	case DOM_VK_ESCAPE:     return KEY_ESCAPE;
	case DOM_VK_SPACE:      return KEY_SPACE;

	case DOM_VK_PAGE_UP:     return KEY_PAGEUP;
	case DOM_VK_PAGE_DOWN:   return KEY_PAGEDOWN;
	case DOM_VK_END:         return KEY_END;
	case DOM_VK_HOME:        return KEY_HOME;
	case DOM_VK_LEFT:        return KEY_LEFT;
	case DOM_VK_UP:          return KEY_UP;
	case DOM_VK_RIGHT:       return KEY_RIGHT;
	case DOM_VK_DOWN:        return KEY_DOWN;
	case DOM_VK_PRINTSCREEN: return KEY_PRINTSCREEN;
	case DOM_VK_INSERT:      return KEY_INSERT;
	case DOM_VK_DELETE:      return KEY_DELETE;

	case DOM_VK_SEMICOLON:   return KEY_SEMICOLON;
	case DOM_VK_EQUALS:      return KEY_EQUALS;
	case DOM_VK_WIN:         return l == DOM_KEY_LOCATION_RIGHT  ? KEY_RWIN : KEY_LWIN;
	case DOM_VK_MULTIPLY:    return KEY_KP_MULTIPLY;
	case DOM_VK_ADD:         return KEY_KP_PLUS;
	case DOM_VK_SUBTRACT:    return KEY_KP_MINUS;
	case DOM_VK_DECIMAL:     return KEY_KP_DECIMAL;
	case DOM_VK_DIVIDE:      return KEY_KP_DIVIDE;
	case DOM_VK_NUM_LOCK:    return KEY_NUMLOCK;
	case DOM_VK_SCROLL_LOCK: return KEY_SCROLLLOCK;
		
	case DOM_VK_HYPHEN_MINUS:  return KEY_MINUS;
	case DOM_VK_COMMA:         return KEY_COMMA;
	case DOM_VK_PERIOD:        return KEY_PERIOD;
	case DOM_VK_SLASH:         return KEY_SLASH;
	case DOM_VK_BACK_QUOTE:    return KEY_TILDE;
	case DOM_VK_OPEN_BRACKET:  return KEY_LBRACKET;
	case DOM_VK_BACK_SLASH:    return KEY_BACKSLASH;
	case DOM_VK_CLOSE_BRACKET: return KEY_RBRACKET;
	case DOM_VK_QUOTE:         return KEY_QUOTE;

	/* chrome */
	case 186: return KEY_SEMICOLON;
	case 187: return KEY_EQUALS;
	case 189: return KEY_MINUS;
	}
	return KEY_NONE;
}

static EM_BOOL OnKeyDown(int type, const EmscriptenKeyboardEvent* ev, void* data) {
	int key = MapNativeKey(ev->keyCode, ev->location);
	/* iOS safari still sends backspace key events, don't intercept those */
	if (key == KEY_BACKSPACE && Input_TouchMode && keyboardOpen) return false;
	
	if (key) Input_SetPressed(key);
	DeferredEnableRawMouse();
	if (!key) return false;

	/* If holding down Ctrl or Alt, keys aren't going to generate a KeyPress event anyways. */
	/* This intercepts Ctrl+S etc. Ctrl+C and Ctrl+V are not intercepted for clipboard. */
	/*  NOTE: macOS uses Win (Command) key instead of Ctrl, have to account for that too */
	if (Key_IsAltPressed())  return true;
	if (Key_IsWinPressed())  return key != 'C' && key != 'V';
	if (Key_IsCtrlPressed()) return key != 'C' && key != 'V';

	/* Space needs special handling, as intercepting this prevents the ' ' key press event */
	/* But on Safari, space scrolls the page - so need to intercept when keyboard is NOT open */
	if (key == KEY_SPACE) return !keyboardOpen;

	/* Must not intercept KeyDown for regular keys, otherwise KeyPress doesn't get raised. */
	/* However, do want to prevent browser's behaviour on F11, F5, home etc. */
	/* e.g. not preventing F11 means browser makes page fullscreen instead of just canvas */
	return (key >= KEY_F1  && key <= KEY_F24)  || (key >= KEY_UP    && key <= KEY_RIGHT) ||
		(key >= KEY_INSERT && key <= KEY_MENU) || (key >= KEY_ENTER && key <= KEY_NUMLOCK);
}

static EM_BOOL OnKeyUp(int type, const EmscriptenKeyboardEvent* ev, void* data) {
	int key = MapNativeKey(ev->keyCode, ev->location);
	if (key) Input_SetReleased(key);
	DeferredEnableRawMouse();
	return key != KEY_NONE;
}

static EM_BOOL OnKeyPress(int type, const EmscriptenKeyboardEvent* ev, void* data) {
	char keyChar;
	DeferredEnableRawMouse();
	/* When on-screen keyboard is open, we don't want to intercept any key presses, */
	/* because they should be sent to the HTML text input instead. */
	/* (Chrome for android sends keypresses sometimes for '0' to '9' keys) */
	/* - If any keys are intercepted, this causes the HTML text input to become */
	/*   desynchronised from the chat/menu input widget the user sees in game. */
	/* - This causes problems such as attempting to backspace all text later to */
	/*   not actually backspace everything. (because the HTML text input does not */
	/*   have these intercepted key presses in its text buffer) */
	if (Input_TouchMode && keyboardOpen) return false;

	/* Safari on macOS still sends a keypress event, which must not be cancelled */
	/*  (otherwise copy/paste doesn't work, as it uses Win+C / Win+V) */
	if (ev->metaKey) return false;

	if (Convert_TryCodepointToCP437(ev->charCode, &keyChar)) {
		Event_RaiseInt(&InputEvents.Press, keyChar);
	}
	return true;
}

/* Really old emscripten versions (e.g. 1.38.21) don't have this defined */
/* Can't just use "#window", newer versions switched to const int instead */
#ifndef EMSCRIPTEN_EVENT_TARGET_WINDOW
#define EMSCRIPTEN_EVENT_TARGET_WINDOW "#window"
#endif

static void HookEvents(void) {
	emscripten_set_wheel_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, NULL, 0, OnMouseWheel);
	emscripten_set_mousedown_callback("#canvas",                  NULL, 0, OnMouseButton);
	emscripten_set_mouseup_callback("#canvas",                    NULL, 0, OnMouseButton);
	emscripten_set_mousemove_callback("#canvas",                  NULL, 0, OnMouseMove);
	emscripten_set_fullscreenchange_callback("#canvas",           NULL, 0, OnFullscreenChange);

	emscripten_set_focus_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW,  NULL, 0, OnFocus);
	emscripten_set_blur_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW,   NULL, 0, OnFocus);
	emscripten_set_resize_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, NULL, 0, OnResize);
	emscripten_set_beforeunload_callback(                          NULL,    OnBeforeUnload);
	emscripten_set_visibilitychange_callback(                      NULL, 0, OnVisibilityChanged);

	emscripten_set_keydown_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW,  NULL, 0, OnKeyDown);
	emscripten_set_keyup_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW,    NULL, 0, OnKeyUp);
	emscripten_set_keypress_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, NULL, 0, OnKeyPress);

	emscripten_set_touchstart_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW,  NULL, 0, OnTouchStart);
	emscripten_set_touchmove_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW,   NULL, 0, OnTouchMove);
	emscripten_set_touchend_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW,    NULL, 0, OnTouchEnd);
	emscripten_set_touchcancel_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, NULL, 0, OnTouchEnd);
}

static void UnhookEvents(void) {
	emscripten_set_wheel_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, NULL, 0, NULL);
	emscripten_set_mousedown_callback("#canvas",                  NULL, 0, NULL);
	emscripten_set_mouseup_callback("#canvas",                    NULL, 0, NULL);
	emscripten_set_mousemove_callback("#canvas",                  NULL, 0, NULL);

	emscripten_set_focus_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW,  NULL, 0, NULL);
	emscripten_set_blur_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW,   NULL, 0, NULL);
	emscripten_set_resize_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, NULL, 0, NULL);
	emscripten_set_beforeunload_callback(                          NULL,    NULL);
	emscripten_set_visibilitychange_callback(                      NULL, 0, NULL);

	emscripten_set_keydown_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW,  NULL, 0, NULL);
	emscripten_set_keyup_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW,    NULL, 0, NULL);
	emscripten_set_keypress_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, NULL, 0, NULL);

	emscripten_set_touchstart_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW,  NULL, 0, NULL);
	emscripten_set_touchmove_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW,   NULL, 0, NULL);
	emscripten_set_touchend_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW,    NULL, 0, NULL);
	emscripten_set_touchcancel_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, NULL, 0, NULL);
}

extern int interop_IsAndroid(void);
extern int interop_IsIOS(void);
extern void interop_AddClipboardListeners(void);
extern void interop_ForceTouchPageLayout(void);

extern void Game_DoFrame(void);
void Window_Init(void) {
	int is_ios, droid;
	emscripten_set_main_loop(Game_DoFrame, 0, false);

	DisplayInfo.Width  = GetScreenWidth();
	DisplayInfo.Height = GetScreenHeight();
	DisplayInfo.Depth  = 24;

	DisplayInfo.ScaleX = emscripten_get_device_pixel_ratio();
	DisplayInfo.ScaleY = DisplayInfo.ScaleX;
	interop_AddClipboardListeners();

	droid  = interop_IsAndroid();
	is_ios = interop_IsIOS();
	Input_SetTouchMode(is_ios || droid);

	/* iOS shifts the whole webpage up when opening chat, which causes problems */
	/*  as the chat/send butons are positioned at the top of the canvas - they */
	/*  get pushed offscreen and can't be used at all anymore. So handle this */
	/*  case specially by positioning them at the bottom instead for iOS. */
	WindowInfo.SoftKeyboard = is_ios ? SOFT_KEYBOARD_SHIFT : SOFT_KEYBOARD_RESIZE;

	/* Let the webpage know it needs to force a mobile layout */
	if (!Input_TouchMode) return;
	interop_ForceTouchPageLayout();
}

extern void interop_InitContainer(void);
static void DoCreateWindow(void) {
	WindowInfo.Exists  = true;
	WindowInfo.Focused = true;
	HookEvents();
	/* Let the webpage decide on initial bounds */
	WindowInfo.Width  = interop_CanvasWidth();
	WindowInfo.Height = interop_CanvasHeight();
	interop_InitContainer();
}
void Window_Create2D(int width, int height) { DoCreateWindow(); }
void Window_Create3D(int width, int height) { DoCreateWindow(); }

extern void interop_SetPageTitle(const char* title);
void Window_SetTitle(const cc_string* title) {
	char str[NATIVE_STR_LEN];
	Platform_EncodeUtf8(str, title);
	interop_SetPageTitle(str);
}

static char pasteBuffer[512];
static cc_string pasteStr;
EMSCRIPTEN_KEEPALIVE void Window_RequestClipboardText(void) {
	Event_RaiseInput(&InputEvents.Down, INPUT_CLIPBOARD_COPY, 0);
}

EMSCRIPTEN_KEEPALIVE void Window_StoreClipboardText(char* src) {
	String_InitArray(pasteStr, pasteBuffer);
	String_AppendUtf8(&pasteStr, src, String_CalcLen(src, 2048));
}

EMSCRIPTEN_KEEPALIVE void Window_GotClipboardText(char* src) {
	Window_StoreClipboardText(src);
	Event_RaiseInput(&InputEvents.Down, INPUT_CLIPBOARD_PASTE, 0);
}

extern void interop_TryGetClipboardText(void);
void Clipboard_GetText(cc_string* value) {
	/* Window_StoreClipboardText may or may not be called by this */
	interop_TryGetClipboardText();
	
	String_Copy(value, &pasteStr);
	pasteStr.length = 0;
}

extern void interop_TrySetClipboardText(const char* text);
void Clipboard_SetText(const cc_string* value) {
	char str[NATIVE_STR_LEN];
	Platform_EncodeUtf8(str, value);
	interop_TrySetClipboardText(str);
}

int Window_GetWindowState(void) {
	EmscriptenFullscreenChangeEvent status = { 0 };
	emscripten_get_fullscreen_status(&status);
	return status.isFullscreen ? WINDOW_STATE_FULLSCREEN : WINDOW_STATE_NORMAL;
}

extern int interop_GetContainerID(void);
extern void interop_EnterFullscreen(void);
cc_result Window_EnterFullscreen(void) {
	EmscriptenFullscreenStrategy strategy;
	const char* target;
	int res;
	strategy.scaleMode                 = EMSCRIPTEN_FULLSCREEN_SCALE_STRETCH;
	strategy.canvasResolutionScaleMode = EMSCRIPTEN_FULLSCREEN_CANVAS_SCALE_HIDEF;
	strategy.filteringMode             = EMSCRIPTEN_FULLSCREEN_FILTERING_DEFAULT;

	strategy.canvasResizedCallback         = OnCanvasResize;
	strategy.canvasResizedCallbackUserData = NULL;

	/* TODO: Return container element ID instead of hardcoding here */
	res    = interop_GetContainerID();
	target = res ? "canvas_wrapper" : "#canvas";

	res = emscripten_request_fullscreen_strategy(target, 1, &strategy);
	if (res == EMSCRIPTEN_RESULT_NOT_SUPPORTED) res = ERR_NOT_SUPPORTED;
	if (res) return res;

	interop_EnterFullscreen();
	return 0;
}

cc_result Window_ExitFullscreen(void) {
	emscripten_exit_fullscreen();
	UpdateWindowBounds();
	return 0;
}

int Window_IsObscured(void) { return 0; }

void Window_Show(void) { }

void Window_SetSize(int width, int height) {
	emscripten_set_canvas_element_size("#canvas", width, height);
	/* CSS size is in CSS units not pixel units */
	emscripten_set_element_css_size("#canvas", width / DisplayInfo.ScaleX, height / DisplayInfo.ScaleY);
	UpdateWindowBounds();
}

void Window_Close(void) {
	WindowInfo.Exists = false;
	Event_RaiseVoid(&WindowEvents.Closing);
	/* If the game is closed while in fullscreen, the last rendered frame stays */
	/*  shown in fullscreen, but the game can't be interacted with anymore */
	Window_ExitFullscreen();

	/* Don't want cursor stuck on the dead 0,0 canvas */
	Window_DisableRawMouse();
	Window_SetSize(0, 0);
	UnhookEvents();
	/* Game_DoFrame doesn't do anything when WindowExists.False is false, */
	/*  but it's still better to cancel main loop to minimise resource usage */
	emscripten_cancel_main_loop();
}

extern void interop_RequestCanvasResize(void);
void Window_ProcessEvents(void) {
	if (!needResize) return;
	needResize = false;
	if (!WindowInfo.Exists) return;

	if (Window_GetWindowState() == WINDOW_STATE_FULLSCREEN) {
		SetFullscreenBounds();
	} else {
		/* Webpage can adjust canvas size if it wants to */
		interop_RequestCanvasResize();
	}
	UpdateWindowBounds();
}

/* Not needed because browser provides relative mouse and touch events */
static void Cursor_GetRawPos(int* x, int* y) { *x = 0; *y = 0; }
/* Not allowed to move cursor from javascript */
void Cursor_SetPosition(int x, int y) { }

extern void interop_SetCursorVisible(int visible);
static void Cursor_DoSetVisible(cc_bool visible) {
	interop_SetCursorVisible(visible);
}

extern void interop_ShowDialog(const char* title, const char* msg);
static void ShowDialogCore(const char* title, const char* msg) { 
	interop_ShowDialog(title, msg); 
}

static OpenFileDialogCallback uploadCallback;
EMSCRIPTEN_KEEPALIVE void Window_OnFileUploaded(const char* src) { 
	cc_string file; char buffer[FILENAME_SIZE];
	String_InitArray(file, buffer);

	String_AppendUtf8(&file, src, String_Length(src));
	uploadCallback(&file);
	uploadCallback = NULL;
}

extern void interop_OpenFileDialog(const char* filter);
cc_result Window_OpenFileDialog(const char* const* filters, OpenFileDialogCallback callback) {
	cc_string path; char pathBuffer[NATIVE_STR_LEN];
	char filter[NATIVE_STR_LEN];
	int i;

	/* Filter tokens are , separated - e.g. ".cw,.dat */
	String_InitArray(path, pathBuffer);
	for (i = 0; filters[i]; i++)
	{
		if (i) String_Append(&path, ',');
		String_AppendConst(&path, filters[i]);
	}
	Platform_EncodeUtf8(filter, &path);

	uploadCallback = callback;
	/* Calls Window_OnFileUploaded on success */
	interop_OpenFileDialog(filter);
	return 0;
}

void Window_AllocFramebuffer(struct Bitmap* bmp) { }
void Window_DrawFramebuffer(Rect2D r)     { }
void Window_FreeFramebuffer(struct Bitmap* bmp)  { }

extern void interop_OpenKeyboard(const char* text, int type, const char* placeholder);
extern void interop_SetKeyboardText(const char* text);
extern void interop_CloseKeyboard(void);

EMSCRIPTEN_KEEPALIVE void Window_OnTextChanged(const char* src) { 
	cc_string str; char buffer[800];
	String_InitArray(str, buffer);

	String_AppendUtf8(&str, src, String_CalcLen(src, 3200));
	Event_RaiseString(&InputEvents.TextChanged, &str);
}

void Window_OpenKeyboard(const struct OpenKeyboardArgs* args) {
	char str[NATIVE_STR_LEN];
	keyboardOpen = true;
	if (!Input_TouchMode) return;

	Platform_EncodeUtf8(str, args->text);
	Platform_LogConst("OPEN SESAME");
	interop_OpenKeyboard(str, args->type, args->placeholder);
}

void Window_SetKeyboardText(const cc_string* text) {
	char str[NATIVE_STR_LEN];
	if (!Input_TouchMode) return;

	Platform_EncodeUtf8(str, text);
	interop_SetKeyboardText(str);
}

void Window_CloseKeyboard(void) {
	keyboardOpen = false;
	if (!Input_TouchMode) return;
	interop_CloseKeyboard();
}

void Window_EnableRawMouse(void) {
	RegrabMouse();
	/* defer pointerlock request until next user input */
	Input_RawMode = true;
}
void Window_UpdateRawMouse(void) { }

void Window_DisableRawMouse(void) {
	RegrabMouse();
	emscripten_exit_pointerlock();
	Input_RawMode = false;
}


/*########################################################################################################################*
*------------------------------------------------Emscripten WebGL context-------------------------------------------------*
*#########################################################################################################################*/
#ifdef CC_BUILD_GL
#include "Graphics.h"
static EMSCRIPTEN_WEBGL_CONTEXT_HANDLE ctx_handle;

static EM_BOOL GLContext_OnLost(int eventType, const void *reserved, void *userData) {
	Gfx_LoseContext("WebGL context lost");
	return 1;
}

void GLContext_Create(void) {
	EmscriptenWebGLContextAttributes attribs;
	emscripten_webgl_init_context_attributes(&attribs);
	attribs.alpha     = false;
	attribs.depth     = true;
	attribs.stencil   = false;
	attribs.antialias = false;

	ctx_handle = emscripten_webgl_create_context("#canvas", &attribs);
	if (!ctx_handle) Window_ShowDialog("WebGL unsupported", "WebGL is required to run ClassiCube");

	emscripten_webgl_make_context_current(ctx_handle);
	emscripten_set_webglcontextlost_callback("#canvas", NULL, 0, GLContext_OnLost);
}

void GLContext_Update(void) {
	/* TODO: do we need to do something here.... ? */
}
cc_bool GLContext_TryRestore(void) {
	return !emscripten_is_webgl_context_lost(0);
}

void GLContext_Free(void) {
	emscripten_webgl_destroy_context(ctx_handle);
	emscripten_set_webglcontextlost_callback("#canvas", NULL, 0, NULL);
}

void* GLContext_GetAddress(const char* function) { return NULL; }
cc_bool GLContext_SwapBuffers(void) { return true; /* Browser implicitly does this */ }

void GLContext_SetFpsLimit(cc_bool vsync, float minFrameMs) {
	if (vsync) {
		emscripten_set_main_loop_timing(EM_TIMING_RAF, 1);
	} else {
		emscripten_set_main_loop_timing(EM_TIMING_SETTIMEOUT, (int)minFrameMs);
	}
}

extern void interop_GetGpuRenderer(char* buffer, int len);
void GLContext_GetApiInfo(cc_string* info) { 
	char buffer[NATIVE_STR_LEN];
	int len;
	interop_GetGpuRenderer(buffer, NATIVE_STR_LEN);

	len = String_CalcLen(buffer, NATIVE_STR_LEN);
	if (!len) return;
	String_AppendConst(info, "GPU: ");
	String_AppendUtf8(info, buffer, len);
}
#endif
#endif
