/*
 *   Copyright © 2008-2010 dragchan <zgchan317@gmail.com>
 *   This file is part of FbTerm.
 *
 *   This program is free software; you can redistribute it and/or
 *   modify it under the terms of the GNU General Public License
 *   as published by the Free Software Foundation; either version 2
 *   of the License, or (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 */

#include <unistd.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/vt.h>
#include <linux/kd.h>
#include <linux/kdev_t.h>
#include <linux/input.h>
#include <linux/vt.h>
#include "input.h"
#include "input_key.h"
#include "fbshell.h"
#include "fbshellman.h"
#include "fbterm.h"
#include "improxy.h"
#include "fbconfig.h"

static const s8 show_cursor[] = "\e[?25h";
static const s8 hide_cursor[] = "\e[?25l";
static const s8 disable_blank[] = "\e[9;0]";
static const s8 enable_blank[] = "\e[9;10]";
static const s8 clear_screen[] = "\e[2J\e[H";

DEFINE_INSTANCE(TtyInput)

class TtyInputVT : public TtyInput, public IoPipe {
	friend class TtyInput;

public:
	void switchVc(bool enter);
	void setRawMode(bool raw, bool force = false);
	void showInfo(bool verbose);
	bool isActive(void);

protected:
	TtyInputVT();
	~TtyInputVT();

private:
	virtual void readyRead(s8 *buf, u32 len);
	void setupSysKey(bool restore);
	void processRawKeys(s8* buf, u32 len);

	bool mRawMode;
	termios oldTm;
	long oldKbMode;
	bool keymapFailure = false;
	bool inited = false;
};

class TtyInputNull : public TtyInput {
	friend class TtyInput;

public:
	void switchVc(bool enter);
	void setRawMode(bool raw, bool force = false);
	void showInfo(bool verbose);

protected:
	TtyInputNull();
};

TtyInput *TtyInput::createInstance()
{
	bool write_only;
	s8 buf[64];

	Config::instance()->getOption("write-only", write_only);
	if (write_only)
		return new TtyInputNull();

	if (ttyname_r(STDIN_FILENO, buf, sizeof(buf))) {
		fprintf(stderr, "stdin isn't a tty!\n");
		return 0;
	}

	if (!strstr(buf, "/dev/tty") && !strstr(buf, "/dev/vc")) {
		fprintf(stderr, "stdin isn't a interactive tty!\n");
		return 0;
	}

	return new TtyInputVT();
}

TtyInputVT::TtyInputVT()
{
	setFd(dup(STDIN_FILENO));

	s32 ret = ::write(STDIN_FILENO, hide_cursor, sizeof(hide_cursor) - 1);
	ret = ::write(STDIN_FILENO, disable_blank, sizeof(disable_blank) - 1);

	struct vt_mode vtm;
	vtm.mode = VT_PROCESS;
	vtm.waitv = 0;
	vtm.relsig = SIGUSR1;
	vtm.acqsig = SIGUSR2;
	vtm.frsig = 0;
	ioctl(STDIN_FILENO, VT_SETMODE, &vtm);

}

TtyInputVT::~TtyInputVT()
{
	if (!inited) return;

	setupSysKey(true);
	ioctl(STDIN_FILENO, KDSKBMODE, oldKbMode);
	tcsetattr(STDIN_FILENO, TCSAFLUSH, &oldTm);

	s32 ret = ::write(STDIN_FILENO, show_cursor, sizeof(show_cursor) - 1);
	ret = ::write(STDIN_FILENO, enable_blank, sizeof(enable_blank) - 1);
	ret = ::write(STDIN_FILENO, clear_screen, sizeof(clear_screen) - 1);
}

bool TtyInput::isActive(void)
{
	return true;
}

void TtyInputVT::showInfo(bool verbose)
{
	if (keymapFailure) {
		printf("[input] can't change kernel keymap table, all shortcuts will NOT work! see SECURITY NOTES section of man page for solution.\n");
	}
}

void TtyInputVT::switchVc(bool enter)
{
	setupSysKey(!enter);

	if (!enter) {
		ioctl(STDIN_FILENO, VT_RELDISP, 1);
		return;
	}

	if (inited)
		return;
	inited = true;

	tcgetattr(STDIN_FILENO, &oldTm);

	ioctl(STDIN_FILENO, KDGKBMODE, &oldKbMode);
	setRawMode(false, true);

	termios tm = oldTm;
	cfmakeraw(&tm);
	tm.c_cc[VMIN] = 1;
	tm.c_cc[VTIME] = 0;
	tcsetattr(STDIN_FILENO, TCSAFLUSH, &tm);
}

void TtyInputVT::setupSysKey(bool restore)
{
	#define T_SHIFT (1 << KG_SHIFT)
	#define T_CTRL (1 << KG_CTRL)
	#define T_CTRL_ALT ((1 << KG_CTRL) + (1 << KG_ALT))

	static bool syskey_saved = false;
	static struct KeyEntry {
		u8 table;
		u8 keycode;
		u16 new_val;
		u16 old_val;
	} sysKeyTable[] = {
		{T_SHIFT,    KEY_PAGEUP,   SHIFT_PAGEUP},
		{T_SHIFT,    KEY_PAGEDOWN, SHIFT_PAGEDOWN},
		{T_SHIFT,    KEY_LEFT,	   SHIFT_LEFT},
		{T_SHIFT,    KEY_RIGHT,    SHIFT_RIGHT},
		{T_CTRL,     KEY_SPACE,    CTRL_SPACE},
		{T_CTRL_ALT, KEY_1,        CTRL_ALT_1},
		{T_CTRL_ALT, KEY_2,        CTRL_ALT_2},
		{T_CTRL_ALT, KEY_3,        CTRL_ALT_3},
		{T_CTRL_ALT, KEY_4,        CTRL_ALT_4},
		{T_CTRL_ALT, KEY_5,        CTRL_ALT_5},
		{T_CTRL_ALT, KEY_6,        CTRL_ALT_6},
		{T_CTRL_ALT, KEY_7,        CTRL_ALT_7},
		{T_CTRL_ALT, KEY_8,	       CTRL_ALT_8},
		{T_CTRL_ALT, KEY_9,        CTRL_ALT_9},
		{T_CTRL_ALT, KEY_0,        CTRL_ALT_0},
		{T_CTRL_ALT, KEY_C,        CTRL_ALT_C},
		{T_CTRL_ALT, KEY_D,        CTRL_ALT_D},
		{T_CTRL_ALT, KEY_E,        CTRL_ALT_E},
		{T_CTRL_ALT, KEY_F1,       CTRL_ALT_F1},
		{T_CTRL_ALT, KEY_F2,       CTRL_ALT_F2},
		{T_CTRL_ALT, KEY_F3,       CTRL_ALT_F3},
		{T_CTRL_ALT, KEY_F4,       CTRL_ALT_F4},
		{T_CTRL_ALT, KEY_F5,       CTRL_ALT_F5},
		{T_CTRL_ALT, KEY_F6,       CTRL_ALT_F6},
		{T_CTRL_ALT, KEY_K,       CTRL_ALT_K},
	};

	if (!syskey_saved && restore) return;

	seteuid(0);

	s8 imapp[128];
	Config::instance()->getOption("input-method", imapp, sizeof(imapp));

	for (u32 i = 0; i < sizeof(sysKeyTable) / sizeof(KeyEntry); i++) {
		if (!imapp[0]
				&& (sysKeyTable[i].new_val == CTRL_SPACE
					|| sysKeyTable[i].new_val == CTRL_ALT_K)) continue;

		kbentry entry;
		entry.kb_table = sysKeyTable[i].table;
		entry.kb_index = sysKeyTable[i].keycode;

		if (!syskey_saved) {
			ioctl(STDIN_FILENO, KDGKBENT, &entry);
			sysKeyTable[i].old_val = entry.kb_value;
		}

		entry.kb_value = (restore ? sysKeyTable[i].old_val : sysKeyTable[i].new_val);
		s32 ret = ioctl(STDIN_FILENO, KDSKBENT, &entry); //should have perm CAP_SYS_TTY_CONFIG
		if (!keymapFailure && ret == -1) keymapFailure = true;
	}

	if (!syskey_saved && !restore) syskey_saved = true;

	seteuid(getuid());
}

void TtyInputVT::readyRead(s8 *buf, u32 len)
{
	if (mRawMode) {
		processRawKeys(buf, len);
		return;
	}

	FbShell *shell = FbShellManager::instance()->activeShell();

	u32 start = 0;
	for (u32 i = 0; i < len; i++) {
		u32 orig = i;
		u16 c = (u8)buf[i];

		if ((c >> 5) == 0x6 && i < (len - 1) && (((u8)buf[++i]) >> 6) == 0x2) {
			c = ((c & 0x1f) << 6) | (buf[i] & 0x3f);
			if (c < AC_START || c > AC_END) continue;

			if (shell && orig > start) shell->keyInput(buf + start, orig - start);
			start = i + 1;

			FbTerm::instance()->processSysKey(c);
		}
	}

	if (shell && len > start) shell->keyInput(buf + start, len - start);
}

static u16 down_num;
static bool key_down[NR_KEYS];
static u8 shift_down[NR_SHIFT];
static u16 shift_state;

void TtyInputVT::setRawMode(bool raw, bool force)
{
	if (!force && raw == mRawMode) return;

	mRawMode = raw;
	ioctl(STDIN_FILENO, KDSKBMODE, mRawMode ? K_MEDIUMRAW : K_UNICODE);

	if (mRawMode) {
		down_num = 0;
		shift_state = 0;
		memset(key_down, 0, sizeof(bool) * NR_KEYS);
		memset(shift_down, 0, sizeof(char) * NR_SHIFT);
	} else {
		if (!down_num) return;

		FbShell *shell = FbShellManager::instance()->activeShell();
		if (!shell) return;

		u32 num = down_num;
		for (u32 i = 0; i < NR_KEYS; i++) {
			if (!key_down[i]) continue;

			s8 code = i | 0x80;
			shell->keyInput(&code, 1);

			if (!--num) break;
		}
	}
}

void TtyInputVT::processRawKeys(s8 *buf, u32 len)
{
	FbShell *shell = FbShellManager::instance()->activeShell();
	u32 start = 0;

	for (u32 i = 0; i < len; i++) {
		bool down = !(buf[i] & 0x80);
		u16 code = buf[i] & 0x7f;
		u32 orig = i;

		if (!code) {
			if (i + 2 >= len) break;

			code = (buf[++i] & 0x7f) << 7;
			code |= buf[++i] & 0x7f;
			if (!(buf[i] & 0x80) || !(buf[i - 1] & 0x80)) continue;
		}

		if (code >= NR_KEYS) continue;

		if (down ^ key_down[code]) {
			if (down) down_num++;
			else down_num--;
		} else if (!down) {
			if (shell && orig > start) shell->keyInput(buf + start, orig - start);
			start = i + 1;
		}

		bool rep = (down && key_down[code]);
		key_down[code] = down;

		struct kbentry ke;
		ke.kb_table = shift_state;
		ke.kb_index = code;

		if (ioctl(STDIN_FILENO, KDGKBENT, &ke) == -1) continue;

		u16 value = KVAL(ke.kb_value);
		u16 syskey = 0, switchvc = 0;

		switch (KTYP(ke.kb_value)) {
		case KT_LATIN:
			if (value >= AC_START && value <= AC_END) syskey = value;
			break;

		case KT_CONS:
			switchvc = value + 1;
			break;

		case KT_SHIFT:
			if (rep || value >= NR_SHIFT) break;
			if (value == KVAL(K_CAPSSHIFT)) value = KVAL(K_SHIFT);

			if (down) shift_down[value]++;
			else if (shift_down[value]) shift_down[value]--;

			if (shift_down[value]) shift_state |= (1 << value);
			else shift_state &= ~(1 << value);

			break;

		default:
			break;
		}

		if (down && (syskey || switchvc)) {
			if (shell && i >= start) shell->keyInput(buf + start, i - start + 1);
			start = i + 1;

			if (syskey) {
				FbTerm::instance()->processSysKey(syskey);
			} else {
				ioctl(STDIN_FILENO, VT_ACTIVATE, switchvc);
			}
		}
	}

	if (shell && len > start) shell->keyInput(buf + start, len - start);
}

bool TtyInputVT::isActive(void)
{
	struct vt_stat vtstat;
	ioctl(STDIN_FILENO, VT_GETSTATE, &vtstat);

	struct stat ttystat;
	fstat(STDIN_FILENO, &ttystat);

	return vtstat.v_active == MINOR(ttystat.st_rdev);
}

void TtyInputNull::switchVc(bool enter)
{
}

void TtyInputNull::setRawMode(bool raw, bool force)
{
}

void TtyInputNull::showInfo(bool verbose)
{
}

TtyInputNull::TtyInputNull()
{
}
