#include "ColorPicker.h"
#include "Config.h"
#include "Logger.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <iterator>
#include <string>
#include <tuple>
#include <vector>
#include <Windows.h>

namespace KeeperForge2 {
	namespace {
		constexpr wchar_t WindowClass[] = L"KeeperForge2ColorPicker";
		constexpr double Pi = 3.14159265358979323846;

		// The layout, in pixels at a 15 pixel font (grown with the player's font, as EditorDialogs does).
		constexpr int ClientWidth = 300;
		constexpr int ClientHeight = 404;
		constexpr int WheelX = 16;
		constexpr int WheelY = 16;
		constexpr int WheelSize = 224;
		constexpr int BarX = 256;
		constexpr int BarY = 16;
		constexpr int BarWidth = 28;
		constexpr int SwatchY = 252;
		constexpr int SwatchHeight = 28;
		constexpr int NumbersY = 292;
		constexpr int HexY = 326;
		constexpr int ButtonsY = 366;

		enum : int { IdRed = 101, IdGreen, IdBlue, IdHex, IdDefault };

		// Hue in degrees, saturation and value from 0 to 1.
		struct Hsv {
			double h = 0.0;
			double s = 0.0;
			double v = 1.0;
		};
		std::uint32_t Rgb(int red, int green, int blue) { return static_cast<std::uint32_t>(red << 16 | green << 8 | blue); }
		int Red(std::uint32_t color) { return static_cast<int>((color >> 16) & 0xFF); }
		int Green(std::uint32_t color) { return static_cast<int>((color >> 8) & 0xFF); }
		int Blue(std::uint32_t color) { return static_cast<int>(color & 0xFF); }
		std::uint32_t ToRgb(const Hsv& hsv) {
			const double h = std::fmod(std::fmod(hsv.h, 360.0) + 360.0, 360.0) / 60.0;
			const double c = hsv.v * hsv.s;
			const double x = c * (1.0 - std::fabs(std::fmod(h, 2.0) - 1.0));
			const double m = hsv.v - c;
			double r = 0, g = 0, b = 0;
			switch (static_cast<int>(h)) {
				case 0: r = c; g = x; break;
				case 1: r = x; g = c; break;
				case 2: g = c; b = x; break;
				case 3: g = x; b = c; break;
				case 4: r = x; b = c; break;
				default: r = c; b = x; break;
			}
			const auto byte = [m](double value) { return std::clamp(static_cast<int>(std::lround((value + m) * 255.0)), 0, 255); };
			return Rgb(byte(r), byte(g), byte(b));
		}
		// From a color; a gray (no hue) or black (no saturation either) keeps the hue and saturation it had, so the wheel's
		// marker does not jump while the numbers pass through them.
		Hsv ToHsv(std::uint32_t color, const Hsv& before) {
			const double r = Red(color) / 255.0, g = Green(color) / 255.0, b = Blue(color) / 255.0;
			const double high = (std::max)({ r, g, b });
			const double low = (std::min)({ r, g, b });
			const double range = high - low;
			Hsv hsv;
			hsv.v = high;
			hsv.s = high > 0.0 ? range / high : before.s;
			if (range <= 0.0) hsv.h = before.h;
			else if (high == r) hsv.h = 60.0 * std::fmod((g - b) / range, 6.0);
			else if (high == g) hsv.h = 60.0 * ((b - r) / range + 2.0);
			else hsv.h = 60.0 * ((r - g) / range + 4.0);
			if (hsv.h < 0.0) hsv.h += 360.0;
			if (high <= 0.0) hsv.s = before.s;
			else if (range <= 0.0) hsv.s = 0.0;
			return hsv;
		}

		struct Theme {
			bool dark = false;
			COLORREF back = 0, text = 0, field = 0, fieldText = 0, button = 0, edge = 0;
			HBRUSH backBrush = nullptr, fieldBrush = nullptr;
		};

		struct State {
			HWND window = nullptr;
			HFONT font = nullptr;
			double scale = 1.0;
			Theme theme;
			HHOOK hook = nullptr;
			int cursorRaised = 0;
			Hsv hsv;
			std::uint32_t color = 0;
			std::uint32_t opened = 0;
			std::uint32_t defaultColor = 0;
			std::function<void(std::uint32_t)> changed;
			std::function<void(std::uint32_t, bool)> closed;
			// While the code itself sets the numbers, their change notices are not the player's typing.
			bool settingFields = false;
			enum class Drag { None, Wheel, Bar } drag = Drag::None;
			// The wheel's picture, made for one size and brightness.
			HBITMAP wheel = nullptr;
			int wheelPixels = 0;
			double wheelValue = -1.0;
		};
		State state;

		int Px(int value) { return static_cast<int>(std::lround(value * state.scale)); }

		void LoadTheme() {
			Theme& theme = state.theme;
			theme.dark = Config::GetString("editor_dark_windows", "on") != "off";
			if (theme.dark) {
				theme.back = RGB(32, 32, 32);
				theme.text = RGB(230, 230, 230);
				theme.field = RGB(45, 45, 45);
				theme.fieldText = RGB(235, 235, 235);
				theme.button = RGB(58, 58, 58);
				theme.edge = RGB(100, 100, 100);
			}
			else {
				theme.back = GetSysColor(COLOR_BTNFACE);
				theme.text = GetSysColor(COLOR_BTNTEXT);
				theme.field = GetSysColor(COLOR_WINDOW);
				theme.fieldText = GetSysColor(COLOR_WINDOWTEXT);
				theme.button = GetSysColor(COLOR_BTNFACE);
				theme.edge = GetSysColor(COLOR_BTNSHADOW);
			}
			theme.backBrush = CreateSolidBrush(theme.back);
			theme.fieldBrush = CreateSolidBrush(theme.field);
		}
		void FreeTheme() {
			if (state.theme.backBrush) DeleteObject(state.theme.backBrush);
			if (state.theme.fieldBrush) DeleteObject(state.theme.fieldBrush);
			state.theme.backBrush = state.theme.fieldBrush = nullptr;
		}
		void DarkTitleBar(HWND window) {
			if (!state.theme.dark) return;
			static auto* set = reinterpret_cast<HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD)>(
				[] { auto* dwm = LoadLibraryW(L"dwmapi.dll"); return dwm != nullptr ? reinterpret_cast<void*>(GetProcAddress(dwm, "DwmSetWindowAttribute")) : nullptr; }());
			if (set == nullptr) return;
			const BOOL on = TRUE;
			if (FAILED(set(window, 20, &on, sizeof(on)))) set(window, 19, &on, sizeof(on));
		}

		// ---- The pictures: the wheel (hue round it, saturation out from the middle, at the brightness chosen) and the bar
		// (the chosen hue and saturation from full brightness at the top to black).
		HBITMAP MakePicture(int width, int height, std::vector<std::uint32_t>& pixels, void*& bits) {
			BITMAPINFO info{};
			info.bmiHeader.biSize = sizeof(info.bmiHeader);
			info.bmiHeader.biWidth = width;
			info.bmiHeader.biHeight = -height;
			info.bmiHeader.biPlanes = 1;
			info.bmiHeader.biBitCount = 32;
			info.bmiHeader.biCompression = BI_RGB;
			HBITMAP picture = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
			if (picture != nullptr) std::memcpy(bits, pixels.data(), pixels.size() * 4);
			return picture;
		}
		void MakeWheel() {
			const int size = Px(WheelSize);
			if (state.wheel != nullptr && state.wheelPixels == size && state.wheelValue == state.hsv.v) return;
			if (state.wheel != nullptr) DeleteObject(state.wheel);
			state.wheel = nullptr;
			const double radius = size / 2.0;
			const COLORREF back = state.theme.back;
			const std::uint32_t backPixel = Rgb(GetRValue(back), GetGValue(back), GetBValue(back));
			std::vector<std::uint32_t> pixels(static_cast<std::size_t>(size) * size, backPixel);
			for (int y = 0; y < size; ++y) {
				for (int x = 0; x < size; ++x) {
					const double dx = x + 0.5 - radius;
					const double dy = y + 0.5 - radius;
					const double distance = std::sqrt(dx * dx + dy * dy);
					if (distance > radius) continue;
					double hue = std::atan2(-dy, dx) * 180.0 / Pi;
					if (hue < 0.0) hue += 360.0;
					const std::uint32_t color = ToRgb({ hue, distance / radius, state.hsv.v });
					// The edge's last pixel is mixed with the background, so the circle is smooth.
					const double edge = std::clamp(radius - distance, 0.0, 1.0);
					const auto mix = [edge](int a, int b) { return static_cast<int>(std::lround(a * edge + b * (1.0 - edge))); };
					pixels[static_cast<std::size_t>(y) * size + x] = Rgb(mix(Red(color), Red(backPixel)), mix(Green(color), Green(backPixel)), mix(Blue(color), Blue(backPixel)));
				}
			}
			void* bits = nullptr;
			state.wheel = MakePicture(size, size, pixels, bits);
			state.wheelPixels = size;
			state.wheelValue = state.hsv.v;
		}
		RECT WheelArea() { return { Px(WheelX), Px(WheelY), Px(WheelX) + Px(WheelSize), Px(WheelY) + Px(WheelSize) }; }
		RECT BarArea() { return { Px(BarX), Px(BarY), Px(BarX) + Px(BarWidth), Px(BarY) + Px(WheelSize) }; }
		RECT SwatchArea(int which) {
			const int width = (Px(BarX + BarWidth) - Px(WheelX)) / 2;
			const int left = Px(WheelX) + which * width;
			return { left, Px(SwatchY), left + width, Px(SwatchY) + Px(SwatchHeight) };
		}
		void Fill(HDC dc, const RECT& area, std::uint32_t color) {
			auto* brush = CreateSolidBrush(RGB(Red(color), Green(color), Blue(color)));
			FillRect(dc, &area, brush);
			DeleteObject(brush);
		}
		void Ring(HDC dc, int x, int y, int radius, COLORREF color, int width) {
			auto* pen = CreatePen(PS_SOLID, width, color);
			auto* oldPen = SelectObject(dc, pen);
			auto* oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
			Ellipse(dc, x - radius, y - radius, x + radius + 1, y + radius + 1);
			SelectObject(dc, oldBrush);
			SelectObject(dc, oldPen);
			DeleteObject(pen);
		}
		void Paint(HDC target) {
			RECT client{};
			GetClientRect(state.window, &client);
			auto* dc = CreateCompatibleDC(target);
			auto* canvas = CreateCompatibleBitmap(target, client.right, client.bottom);
			auto* oldCanvas = SelectObject(dc, canvas);
			FillRect(dc, &client, state.theme.backBrush);
			// The wheel and where the color is on it.
			MakeWheel();
			const RECT wheel = WheelArea();
			if (state.wheel != nullptr) {
				auto* pictures = CreateCompatibleDC(dc);
				auto* old = SelectObject(pictures, state.wheel);
				BitBlt(dc, wheel.left, wheel.top, state.wheelPixels, state.wheelPixels, pictures, 0, 0, SRCCOPY);
				SelectObject(pictures, old);
				DeleteDC(pictures);
			}
			const double radius = state.wheelPixels / 2.0;
			const double angle = state.hsv.h * Pi / 180.0;
			const int markX = wheel.left + static_cast<int>(std::lround(radius + std::cos(angle) * state.hsv.s * radius));
			const int markY = wheel.top + static_cast<int>(std::lround(radius - std::sin(angle) * state.hsv.s * radius));
			Ring(dc, markX, markY, Px(6), RGB(0, 0, 0), Px(3));
			Ring(dc, markX, markY, Px(6), RGB(255, 255, 255), (std::max)(1, Px(1)));
			// The brightness bar, drawn a band at a time, and where the brightness is on it.
			const RECT bar = BarArea();
			const int height = bar.bottom - bar.top;
			for (int y = 0; y < height; ++y) {
				const double value = 1.0 - static_cast<double>(y) / (std::max)(1, height - 1);
				const RECT row{ bar.left, bar.top + y, bar.right, bar.top + y + 1 };
				Fill(dc, row, ToRgb({ state.hsv.h, state.hsv.s, value }));
			}
			const int barMark = bar.top + static_cast<int>(std::lround((1.0 - state.hsv.v) * (height - 1)));
			auto* pen = CreatePen(PS_SOLID, (std::max)(1, Px(2)), state.hsv.v > 0.5 ? RGB(0, 0, 0) : RGB(255, 255, 255));
			auto* oldPen = SelectObject(dc, pen);
			MoveToEx(dc, bar.left - Px(3), barMark, nullptr);
			LineTo(dc, bar.right + Px(3), barMark);
			SelectObject(dc, oldPen);
			DeleteObject(pen);
			// Before and now.
			const RECT before = SwatchArea(0);
			const RECT now = SwatchArea(1);
			Fill(dc, before, state.opened);
			Fill(dc, now, state.color);
			SetBkMode(dc, TRANSPARENT);
			auto* oldFont = SelectObject(dc, state.font);
			for (const auto& [area, label, color] : { std::tuple{ before, L"Before", state.opened }, std::tuple{ now, L"Now", state.color } }) {
				// Text that shows on the swatch's own color.
				const double light = 0.299 * Red(color) + 0.587 * Green(color) + 0.114 * Blue(color);
				SetTextColor(dc, light > 140.0 ? RGB(0, 0, 0) : RGB(255, 255, 255));
				RECT text = area;
				DrawTextW(dc, label, -1, &text, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
			}
			SelectObject(dc, oldFont);
			BitBlt(target, 0, 0, client.right, client.bottom, dc, 0, 0, SRCCOPY);
			SelectObject(dc, oldCanvas);
			DeleteObject(canvas);
			DeleteDC(dc);
		}

		// ---- Keeping the parts in step.
		HWND Item(int id) { return GetDlgItem(state.window, id); }
		void SetFields(int except) {
			state.settingFields = true;
			const int values[3] = { Red(state.color), Green(state.color), Blue(state.color) };
			for (int i = 0; i < 3; ++i) {
				if (IdRed + i == except) continue;
				SetWindowTextW(Item(IdRed + i), std::to_wstring(values[i]).c_str());
			}
			if (except != IdHex) SetWindowTextW(Item(IdHex), ColorPicker::Hex(state.color).c_str());
			state.settingFields = false;
		}
		// The color is now this: the other parts follow, the window is drawn again, and the owner is told.
		void Changed(int from) {
			SetFields(from);
			InvalidateRect(state.window, nullptr, FALSE);
			if (state.changed) state.changed(state.color);
		}
		void SetFromHsv(const Hsv& hsv) {
			state.hsv = hsv;
			state.color = ToRgb(hsv);
			Changed(0);
		}
		void SetFromRgb(std::uint32_t color, int from) {
			state.color = color & 0xFFFFFF;
			state.hsv = ToHsv(state.color, state.hsv);
			Changed(from);
		}
		std::wstring Text(int id) {
			wchar_t text[32]{};
			GetWindowTextW(Item(id), text, static_cast<int>(std::size(text)));
			return text;
		}
		void NumbersTyped(int id) {
			int values[3]{};
			for (int i = 0; i < 3; ++i) {
				const auto text = Text(IdRed + i);
				if (text.empty()) return;
				wchar_t* end = nullptr;
				const long value = std::wcstol(text.c_str(), &end, 10);
				if (end == nullptr || *end != 0 || value < 0 || value > 255) return;
				values[i] = static_cast<int>(value);
			}
			SetFromRgb(Rgb(values[0], values[1], values[2]), id);
		}
		void HexTyped() {
			std::uint32_t color = 0;
			if (ColorPicker::ParseHex(Text(IdHex), color)) SetFromRgb(color, IdHex);
		}
		void DragTo(int x, int y) {
			if (state.drag == State::Drag::Wheel) {
				const RECT wheel = WheelArea();
				const double radius = state.wheelPixels / 2.0;
				const double dx = x - (wheel.left + radius);
				const double dy = y - (wheel.top + radius);
				double hue = std::atan2(-dy, dx) * 180.0 / Pi;
				if (hue < 0.0) hue += 360.0;
				SetFromHsv({ hue, (std::min)(1.0, std::sqrt(dx * dx + dy * dy) / radius), state.hsv.v });
			}
			else if (state.drag == State::Drag::Bar) {
				const RECT bar = BarArea();
				const double value = 1.0 - static_cast<double>(y - bar.top) / (std::max)(1L, bar.bottom - bar.top - 1);
				SetFromHsv({ state.hsv.h, state.hsv.s, std::clamp(value, 0.0, 1.0) });
			}
		}

		void Finish(bool kept) {
			if (state.window == nullptr) return;
			const auto closed = state.closed;
			const auto color = kept ? state.color : state.opened;
			state.closed = nullptr;
			state.changed = nullptr;
			DestroyWindow(state.window);
			if (closed) closed(color, kept);
		}

		HWND Make(const wchar_t* windowClass, const wchar_t* text, DWORD style, int x, int y, int width, int height, int id, DWORD extended = 0) {
			auto* control = CreateWindowExW(extended, windowClass, text, WS_CHILD | WS_VISIBLE | style, Px(x), Px(y), Px(width), Px(height), state.window,
				reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), nullptr, nullptr);
			if (control != nullptr) SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(state.font), FALSE);
			return control;
		}
		HWND Field(int x, int y, int width, int id, int limit, bool number) {
			auto* field = Make(L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP | (number ? ES_NUMBER : 0), x, y, width, 22, id, state.theme.dark ? 0 : WS_EX_CLIENTEDGE);
			if (field != nullptr) SendMessageW(field, EM_SETLIMITTEXT, static_cast<WPARAM>(limit), 0);
			return field;
		}
		void Button(const wchar_t* text, int x, int width, int id, bool main = false) {
			const DWORD kind = state.theme.dark ? BS_OWNERDRAW : (main ? BS_DEFPUSHBUTTON : BS_PUSHBUTTON);
			Make(L"BUTTON", text, kind | WS_TABSTOP, x, ButtonsY, width, 26, id);
		}
		// Dark push buttons: the classic ones cannot be colored, so they are drawn here.
		bool DrawButton(const DRAWITEMSTRUCT& item) {
			if (item.CtlType != ODT_BUTTON) return false;
			const bool pressed = (item.itemState & ODS_SELECTED) != 0;
			const bool focused = (item.itemState & ODS_FOCUS) != 0;
			auto* brush = CreateSolidBrush(pressed ? RGB(80, 80, 80) : state.theme.button);
			FillRect(item.hDC, &item.rcItem, brush);
			DeleteObject(brush);
			auto* edge = CreateSolidBrush(focused ? RGB(160, 160, 160) : state.theme.edge);
			FrameRect(item.hDC, &item.rcItem, edge);
			DeleteObject(edge);
			wchar_t text[64]{};
			GetWindowTextW(item.hwndItem, text, static_cast<int>(std::size(text)));
			SetBkMode(item.hDC, TRANSPARENT);
			SetTextColor(item.hDC, state.theme.text);
			RECT area = item.rcItem;
			DrawTextW(item.hDC, text, -1, &area, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
			return true;
		}

		LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
			switch (message) {
				case WM_COMMAND: {
					const int id = LOWORD(wParam);
					const int code = HIWORD(wParam);
					if (id == IDOK && code == BN_CLICKED) Finish(true);
					else if (id == IDCANCEL && code == BN_CLICKED) Finish(false);
					else if (id == IdDefault && code == BN_CLICKED) SetFromRgb(state.defaultColor, 0);
					else if (code == EN_CHANGE && !state.settingFields) {
						if (id == IdHex) HexTyped();
						else if (id >= IdRed && id <= IdBlue) NumbersTyped(id);
					}
					else if (code == EN_KILLFOCUS) SetFields(0);
					return 0;
				}
				case WM_LBUTTONDOWN: {
					const POINT point{ static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam)) };
					const RECT wheel = WheelArea();
					const double radius = state.wheelPixels / 2.0;
					const double dx = point.x - (wheel.left + radius);
					const double dy = point.y - (wheel.top + radius);
					RECT bar = BarArea();
					InflateRect(&bar, Px(4), 0);
					if (std::sqrt(dx * dx + dy * dy) <= radius + Px(2)) state.drag = State::Drag::Wheel;
					else if (PtInRect(&bar, point)) state.drag = State::Drag::Bar;
					else break;
					SetCapture(window);
					DragTo(point.x, point.y);
					return 0;
				}
				case WM_MOUSEMOVE:
					if (state.drag != State::Drag::None) DragTo(static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam)));
					return 0;
				case WM_LBUTTONUP:
				case WM_CAPTURECHANGED:
					if (state.drag != State::Drag::None) {
						state.drag = State::Drag::None;
						if (message == WM_LBUTTONUP) ReleaseCapture();
					}
					return 0;
				case WM_CTLCOLORSTATIC:
				case WM_CTLCOLOREDIT: {
					auto* dc = reinterpret_cast<HDC>(wParam);
					const bool field = message == WM_CTLCOLOREDIT;
					SetTextColor(dc, field ? state.theme.fieldText : state.theme.text);
					SetBkColor(dc, field ? state.theme.field : state.theme.back);
					return reinterpret_cast<LRESULT>(field ? state.theme.fieldBrush : state.theme.backBrush);
				}
				case WM_DRAWITEM:
					if (DrawButton(*reinterpret_cast<const DRAWITEMSTRUCT*>(lParam))) return TRUE;
					break;
				case DM_GETDEFID:
					return MAKELRESULT(IDOK, DC_HASDEFID);
				case WM_ERASEBKGND:
					return TRUE;
				case WM_PAINT: {
					PAINTSTRUCT paint{};
					auto* dc = BeginPaint(window, &paint);
					Paint(dc);
					EndPaint(window, &paint);
					return 0;
				}
				case WM_SETCURSOR: {
					// The game hides Windows' pointer on its thread, which is this window's too. Raised here, put back on closing.
					CURSORINFO info{ sizeof(info) };
					if (GetCursorInfo(&info) && (info.flags & CURSOR_SHOWING) == 0) {
						do { ++state.cursorRaised; } while (ShowCursor(TRUE) < 0);
					}
					break;
				}
				case WM_CLOSE:
					Finish(false);
					return 0;
				case WM_DESTROY:
					if (state.hook != nullptr) UnhookWindowsHookEx(state.hook);
					state.hook = nullptr;
					for (; state.cursorRaised > 0; --state.cursorRaised) ShowCursor(FALSE);
					if (state.wheel != nullptr) DeleteObject(state.wheel);
					state.wheel = nullptr;
					state.window = nullptr;
					return 0;
				case WM_NCDESTROY:
					if (state.font != nullptr) DeleteObject(state.font);
					state.font = nullptr;
					FreeTheme();
					break;
			}
			return DefWindowProcW(window, message, wParam, lParam);
		}
		// The game's message loop has no IsDialogMessage, so Tab, Enter and Esc are given to this window here.
		LRESULT CALLBACK GetMessageHook(int code, WPARAM wParam, LPARAM lParam) {
			if (code == HC_ACTION && wParam == PM_REMOVE && state.window != nullptr) {
				auto* message = reinterpret_cast<MSG*>(lParam);
				if (message->message >= WM_KEYFIRST && message->message <= WM_KEYLAST && (message->hwnd == state.window || IsChild(state.window, message->hwnd))
					&& IsDialogMessageW(state.window, message)) {
					message->message = WM_NULL;
					message->wParam = 0;
					message->lParam = 0;
				}
			}
			return CallNextHookEx(nullptr, code, wParam, lParam);
		}
		HINSTANCE OurModule() {
			HMODULE module = nullptr;
			GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&WindowProc), &module);
			return module;
		}
		bool Register() {
			static bool registered = false;
			if (registered) return true;
			WNDCLASSEXW windowClass{ sizeof(windowClass) };
			windowClass.lpfnWndProc = &WindowProc;
			windowClass.hInstance = OurModule();
			windowClass.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)); // IDC_ARROW
			windowClass.lpszClassName = WindowClass;
			registered = RegisterClassExW(&windowClass) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
			return registered;
		}
	}

	std::wstring ColorPicker::Hex(std::uint32_t color) {
		wchar_t text[8]{};
		std::swprintf(text, std::size(text), L"#%06X", color & 0xFFFFFF);
		return text;
	}
	bool ColorPicker::ParseHex(const std::wstring& text, std::uint32_t& color) {
		std::wstring digits;
		for (const wchar_t c : text) if (!std::iswspace(c)) digits += c;
		if (!digits.empty() && digits.front() == L'#') digits.erase(digits.begin());
		if (digits.size() != 6 || !std::all_of(digits.begin(), digits.end(), [](wchar_t c) { return std::iswxdigit(c) != 0; })) return false;
		color = static_cast<std::uint32_t>(std::wcstoul(digits.c_str(), nullptr, 16));
		return true;
	}

	bool ColorPicker::IsOpen() { return state.window != nullptr; }
	void ColorPicker::Close() { Finish(false); }

	bool ColorPicker::Open(const std::wstring& title, std::uint32_t color, std::uint32_t defaultColor, std::function<void(std::uint32_t)> changed,
		std::function<void(std::uint32_t, bool)> closed) {
		if (state.window != nullptr) {
			ShowWindow(state.window, SW_SHOWNORMAL);
			SetForegroundWindow(state.window);
			return false;
		}
		if (!Register()) {
			Logger::Warning("Color picker: could not register its window class (" + std::to_string(GetLastError()) + ").");
			return false;
		}
		state = State{};
		LoadTheme();
		state.color = state.opened = color & 0xFFFFFF;
		state.defaultColor = defaultColor & 0xFFFFFF;
		state.hsv = ToHsv(state.color, Hsv{});
		state.changed = std::move(changed);
		state.closed = std::move(closed);
		NONCLIENTMETRICSW metrics{ sizeof(metrics) };
		state.font = SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0) ? CreateFontIndirectW(&metrics.lfMessageFont)
			: static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
		if (auto* screen = GetDC(nullptr)) {
			auto* old = SelectObject(screen, state.font);
			TEXTMETRICW text{};
			if (GetTextMetricsW(screen, &text) && text.tmHeight > 0) state.scale = (std::max)(1.0, text.tmHeight / 15.0);
			SelectObject(screen, old);
			ReleaseDC(nullptr, screen);
		}
		constexpr DWORD Style = WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN;
		constexpr DWORD Extended = WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT;
		RECT frame{ 0, 0, Px(ClientWidth), Px(ClientHeight) };
		AdjustWindowRectEx(&frame, Style, FALSE, Extended);
		const int width = frame.right - frame.left;
		const int height = frame.bottom - frame.top;
		// Over the game's window (the active one on this thread: the click that opens this came to it).
		auto* owner = GetActiveWindow();
		RECT area{};
		if (owner == nullptr || !GetWindowRect(owner, &area)) SystemParametersInfoW(SPI_GETWORKAREA, 0, &area, 0);
		const int x = area.left + (std::max)(0L, (area.right - area.left - width) / 2);
		const int y = area.top + (std::max)(0L, (area.bottom - area.top - height) / 2);
		state.window = CreateWindowExW(Extended, WindowClass, title.c_str(), Style, x, y, width, height, owner, nullptr, OurModule(), nullptr);
		if (state.window == nullptr) {
			Logger::Warning("Color picker: could not make its window (" + std::to_string(GetLastError()) + ").");
			if (state.font != nullptr) DeleteObject(state.font);
			FreeTheme();
			state = State{};
			return false;
		}
		DarkTitleBar(state.window);
		// Red, green and blue (0-255), then the hex code.
		const wchar_t* names[3] = { L"R", L"G", L"B" };
		for (int i = 0; i < 3; ++i) {
			const int left = 16 + i * 92;
			Make(L"STATIC", names[i], SS_RIGHT, left, NumbersY + 3, 14, 18, -1);
			Field(left + 20, NumbersY, 58, IdRed + i, 3, true);
		}
		Make(L"STATIC", L"Hex", SS_RIGHT, 16, HexY + 3, 34, 18, -1);
		Field(56, HexY, 98, IdHex, 7, false);
		Button(L"Game's color", 16, 108, IdDefault);
		Button(L"OK", 136, 70, IDOK, true);
		Button(L"Cancel", 214, 70, IDCANCEL);
		SetFields(0);
		state.hook = SetWindowsHookExW(WH_GETMESSAGE, &GetMessageHook, nullptr, GetCurrentThreadId());
		ShowWindow(state.window, SW_SHOWNORMAL);
		SetForegroundWindow(state.window);
		SetFocus(Item(IdHex));
		return true;
	}
}
