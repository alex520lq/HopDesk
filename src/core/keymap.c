/* =====================================================================
 *  HopDesk - 键码映射（平台无关，阶段 2）
 *
 *  Windows 虚拟键码（VK_*）-> macOS 虚拟键码（kVK_*）。
 *
 *  映射策略（固定双设备、自用，不做可配置）：
 *    物理 Ctrl  ->  macOS Command   （沿用 Deskflow 的 modifierCtrl=meta）
 *    物理 Alt   ->  macOS Option
 *    物理 Win   ->  macOS Control   （让四个 Mac 修饰键都可达）
 *    Shift      ->  Shift
 *
 *  表按 US 布局建立。两端布局不同（例如 Windows 用德语键盘）时，
 *  字符键会错位 —— 这是 VK 映射的固有限制，不是 bug。
 *  真正的解法是改成扫描码映射（物理位置），但会牺牲 VK_OEM_* 的语义，
 *  等真的用出问题再换。
 * ===================================================================== */
#include "hop.h"

/* macOS kVK_*（HIToolbox/Events.h），只抄用得到的部分 */
enum {
    MK_A = 0x00, MK_S = 0x01, MK_D = 0x02, MK_F = 0x03, MK_H = 0x04,
    MK_G = 0x05, MK_Z = 0x06, MK_X = 0x07, MK_C = 0x08, MK_V = 0x09,
    MK_B = 0x0B, MK_Q = 0x0C, MK_W = 0x0D, MK_E = 0x0E, MK_R = 0x0F,
    MK_Y = 0x10, MK_T = 0x11, MK_1 = 0x12, MK_2 = 0x13, MK_3 = 0x14,
    MK_4 = 0x15, MK_6 = 0x16, MK_5 = 0x17, MK_EQUAL = 0x18, MK_9 = 0x19,
    MK_7 = 0x1A, MK_MINUS = 0x1B, MK_8 = 0x1C, MK_0 = 0x1D,
    MK_RBRACKET = 0x1E, MK_O = 0x1F, MK_U = 0x20, MK_LBRACKET = 0x21,
    MK_I = 0x22, MK_P = 0x23, MK_RETURN = 0x24, MK_L = 0x25, MK_J = 0x26,
    MK_QUOTE = 0x27, MK_K = 0x28, MK_SEMICOLON = 0x29, MK_BACKSLASH = 0x2A,
    MK_COMMA = 0x2B, MK_SLASH = 0x2C, MK_N = 0x2D, MK_M = 0x2E,
    MK_PERIOD = 0x2F, MK_TAB = 0x30, MK_SPACE = 0x31, MK_GRAVE = 0x32,
    MK_DELETE = 0x33, MK_ESCAPE = 0x35, MK_RIGHTCMD = 0x36, MK_CMD = 0x37,
    MK_SHIFT = 0x38, MK_CAPS = 0x39, MK_OPTION = 0x3A, MK_CONTROL = 0x3B,
    MK_RSHIFT = 0x3C, MK_ROPTION = 0x3D, MK_RCONTROL = 0x3E,
    MK_KP_DECIMAL = 0x41, MK_KP_MULTIPLY = 0x43, MK_KP_PLUS = 0x45,
    MK_KP_CLEAR = 0x47, MK_VOLUP = 0x48, MK_VOLDOWN = 0x49, MK_MUTE = 0x4A,
    MK_KP_DIVIDE = 0x4B, MK_KP_ENTER = 0x4C, MK_KP_MINUS = 0x4E,
    MK_KP_EQUAL = 0x51, MK_KP0 = 0x52, MK_KP1 = 0x53, MK_KP2 = 0x54,
    MK_KP3 = 0x55, MK_KP4 = 0x56, MK_KP5 = 0x57, MK_KP6 = 0x58,
    MK_KP7 = 0x59, MK_KP8 = 0x5B, MK_KP9 = 0x5C,
    MK_F1 = 0x7A, MK_F2 = 0x78, MK_F3 = 0x63, MK_F4 = 0x76, MK_F5 = 0x60,
    MK_F6 = 0x61, MK_F7 = 0x62, MK_F8 = 0x64, MK_F9 = 0x65, MK_F10 = 0x6D,
    MK_F11 = 0x67, MK_F12 = 0x6F, MK_F13 = 0x69, MK_F14 = 0x6B,
    MK_F15 = 0x71, MK_F16 = 0x6A, MK_F17 = 0x40, MK_F18 = 0x4F,
    MK_F19 = 0x50, MK_F20 = 0x5A, MK_HELP = 0x72, MK_HOME = 0x73,
    MK_PAGEUP = 0x74, MK_FWDDEL = 0x75, MK_END = 0x77, MK_PAGEDOWN = 0x79,
    MK_LEFT = 0x7B, MK_RIGHT = 0x7C, MK_DOWN = 0x7D, MK_UP = 0x7E
};

/* 'A'..'Z' 的 VK 是连续的，mac 键码不是 —— 用表消掉分支 */
static const uint8_t kLetter[26] = {
    MK_A, MK_B, MK_C, MK_D, MK_E, MK_F, MK_G, MK_H, MK_I, MK_J, MK_K, MK_L,
    MK_M, MK_N, MK_O, MK_P, MK_Q, MK_R, MK_S, MK_T, MK_U, MK_V, MK_W, MK_X,
    MK_Y, MK_Z
};
static const uint8_t kDigit[10] = {
    MK_0, MK_1, MK_2, MK_3, MK_4, MK_5, MK_6, MK_7, MK_8, MK_9
};
static const uint8_t kFKey[20] = {
    MK_F1, MK_F2, MK_F3, MK_F4, MK_F5, MK_F6, MK_F7, MK_F8, MK_F9, MK_F10,
    MK_F11, MK_F12, MK_F13, MK_F14, MK_F15, MK_F16, MK_F17, MK_F18, MK_F19,
    MK_F20
};
static const uint8_t kKeypad[10] = {
    MK_KP0, MK_KP1, MK_KP2, MK_KP3, MK_KP4, MK_KP5, MK_KP6, MK_KP7,
    MK_KP8, MK_KP9
};

uint16_t hop_vk_to_mac(int vk) {
    if (vk >= 'A' && vk <= 'Z')   return kLetter[vk - 'A'];
    if (vk >= '0' && vk <= '9')   return kDigit[vk - '0'];
    if (vk >= 0x70 && vk <= 0x83) return kFKey[vk - 0x70];    /* VK_F1..VK_F20 */
    if (vk >= 0x60 && vk <= 0x69) return kKeypad[vk - 0x60];  /* VK_NUMPAD0..9 */

    switch (vk) {
        /* 编辑键 */
        case 0x08: return MK_DELETE;      /* VK_BACK      -> Backspace */
        case 0x09: return MK_TAB;
        case 0x0D: return MK_RETURN;
        case 0x1B: return MK_ESCAPE;
        case 0x20: return MK_SPACE;
        case 0x21: return MK_PAGEUP;      /* VK_PRIOR */
        case 0x22: return MK_PAGEDOWN;    /* VK_NEXT  */
        case 0x23: return MK_END;
        case 0x24: return MK_HOME;
        case 0x25: return MK_LEFT;
        case 0x26: return MK_UP;
        case 0x27: return MK_RIGHT;
        case 0x28: return MK_DOWN;
        case 0x2D: return MK_HELP;        /* VK_INSERT -> Help */
        case 0x2E: return MK_FWDDEL;      /* VK_DELETE -> Forward Delete */

        /* 修饰键：Ctrl -> Command，Alt -> Option，Win -> Control */
        case 0xA0: return MK_SHIFT;       /* VK_LSHIFT   */
        case 0xA1: return MK_RSHIFT;      /* VK_RSHIFT   */
        case 0xA2: return MK_CMD;         /* VK_LCONTROL -> Command */
        case 0xA3: return MK_RIGHTCMD;    /* VK_RCONTROL -> Command */
        case 0xA4: return MK_OPTION;      /* VK_LMENU    */
        case 0xA5: return MK_ROPTION;     /* VK_RMENU    */
        case 0x5B: return MK_CONTROL;     /* VK_LWIN     -> Control */
        case 0x5C: return MK_RCONTROL;    /* VK_RWIN     -> Control */
        case 0x10: return MK_SHIFT;       /* VK_SHIFT   （通用） */
        case 0x11: return MK_CMD;         /* VK_CONTROL （通用） */
        case 0x12: return MK_OPTION;      /* VK_MENU    （通用） */
        case 0x14: return MK_CAPS;        /* VK_CAPITAL */

        /* 标点（US 布局） */
        case 0xBA: return MK_SEMICOLON;   /* VK_OEM_1      ;: */
        case 0xBB: return MK_EQUAL;       /* VK_OEM_PLUS   =+ */
        case 0xBC: return MK_COMMA;       /* VK_OEM_COMMA  ,< */
        case 0xBD: return MK_MINUS;       /* VK_OEM_MINUS  -_ */
        case 0xBE: return MK_PERIOD;      /* VK_OEM_PERIOD .> */
        case 0xBF: return MK_SLASH;       /* VK_OEM_2      /? */
        case 0xC0: return MK_GRAVE;       /* VK_OEM_3      `~ */
        case 0xDB: return MK_LBRACKET;    /* VK_OEM_4      [{ */
        case 0xDC: return MK_BACKSLASH;   /* VK_OEM_5      \| */
        case 0xDD: return MK_RBRACKET;    /* VK_OEM_6      ]} */
        case 0xDE: return MK_QUOTE;       /* VK_OEM_7      '" */

        /* 小键盘运算键 */
        case 0x6A: return MK_KP_MULTIPLY; /* VK_MULTIPLY */
        case 0x6B: return MK_KP_PLUS;     /* VK_ADD      */
        case 0x6D: return MK_KP_MINUS;    /* VK_SUBTRACT */
        case 0x6E: return MK_KP_DECIMAL;  /* VK_DECIMAL  */
        case 0x6F: return MK_KP_DIVIDE;   /* VK_DIVIDE   */
        case 0x90: return MK_KP_CLEAR;    /* VK_NUMLOCK  -> Clear */

        /* 音量（Mac 笔记本键盘上才有，方便顺手） */
        case 0xAD: return MK_MUTE;        /* VK_VOLUME_MUTE  */
        case 0xAE: return MK_VOLDOWN;     /* VK_VOLUME_DOWN  */
        case 0xAF: return MK_VOLUP;       /* VK_VOLUME_UP    */

        default: return HOP_KEY_NONE;
    }
}

int hop_mac_key_is_mod(uint16_t kc) {
    switch (kc) {
        case MK_SHIFT:
        case MK_RSHIFT:   return HOP_MOD_SHIFT;
        case MK_CONTROL:
        case MK_RCONTROL: return HOP_MOD_CONTROL;
        case MK_OPTION:
        case MK_ROPTION:  return HOP_MOD_ALT;
        case MK_CMD:
        case MK_RIGHTCMD: return HOP_MOD_COMMAND;
        case MK_CAPS:     return HOP_MOD_CAPS;
        default:          return 0;
    }
}
