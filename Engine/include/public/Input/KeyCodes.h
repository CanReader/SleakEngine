 #ifndef _KEYCODES_H_
 #define _KEYCODES_H_

#include <string>

 namespace Sleak {
    namespace Input {

    enum class MOUSE_CODE {
        Button0 = 0,
        Button1 = 1,
        Button2 = 2,
        Button3 = 3,
        Button4 = 4,
        Button5 = 5,
        Button6 = 6,
        Button7 = 7,

        ButtonLeft = Button0,
        ButtonMiddle = Button2,
        ButtonRight = Button3,
    };

     enum class KEY_CODE
     {
     KEY__UNKNOWN = 0,
     
     KEY__A = 4,
     KEY__B = 5,
     KEY__C = 6,
     KEY__D = 7,
     KEY__E = 8,
     KEY__F = 9,
     KEY__G = 10,
     KEY__H = 11,
     KEY__I = 12,
     KEY__J = 13,
     KEY__K = 14,
     KEY__L = 15,
     KEY__M = 16,
     KEY__N = 17,
     KEY__O = 18,
     KEY__P = 19,
     KEY__Q = 20,
     KEY__R = 21,
     KEY__S = 22,
     KEY__T = 23,
     KEY__U = 24,
     KEY__V = 25,
     KEY__W = 26,
     KEY__X = 27,
     KEY__Y = 28,
     KEY__Z = 29,
     
     KEY__1 = 30,
     KEY__2 = 31,
     KEY__3 = 32,
     KEY__4 = 33,
     KEY__5 = 34,
     KEY__6 = 35,
     KEY__7 = 36,
     KEY__8 = 37,
     KEY__9 = 38,
     KEY__0 = 39,
     
     KEY__RETURN = 40,
     KEY__ESCAPE = 41,
     KEY__BACKSPACE = 42,
     KEY__TAB = 43,
     KEY__SPACE = 44,
     
     KEY__MINUS = 45,
     KEY__EQUALS = 46,
     KEY__LEFTBRACKET = 47,
     KEY__RIGHTBRACKET = 48,
     KEY__BACKSLASH = 49,
    
    KEY__NONUSHASH = 50, 

     KEY__SEMICOLON = 51,
     KEY__APOSTROPHE = 52,
     KEY__GRAVE = 53, 
     KEY__COMMA = 54,
     KEY__PERIOD = 55,
     KEY__SLASH = 56,
     
     KEY__CAPSLOCK = 57,
 
     KEY__F1 = 58,
     KEY__F2 = 59,
     KEY__F3 = 60,
     KEY__F4 = 61,
     KEY__F5 = 62,
     KEY__F6 = 63,
     KEY__F7 = 64,
     KEY__F8 = 65,
     KEY__F9 = 66,
     KEY__F10 = 67,
     KEY__F11 = 68,
     KEY__F12 = 69,
     
     KEY__PRINTSCREEN = 70,
     KEY__SCROLLLOCK = 71,
     KEY__PAUSE = 72,
     KEY__INSERT = 73,
     KEY__HOME = 74,
     KEY__PAGEUP = 75,
     KEY__DELETE = 76,
     KEY__END = 77,
     KEY__PAGEDOWN = 78,
     KEY__RIGHT = 79,
     KEY__LEFT = 80,
     KEY__DOWN = 81,
     KEY__UP = 82,
     
    KEY__NUMLOCKCLEAR = 83,

    KEY__KP_DIVIDE = 84,
    KEY__KP_MULTIPLY = 85,
    KEY__KP_MINUS = 86,
    KEY__KP_PLUS = 87,
    KEY__KP_ENTER = 88,
    KEY__KP_1 = 89,
    KEY__KP_2 = 90,
    KEY__KP_3 = 91,
    KEY__KP_4 = 92,
    KEY__KP_5 = 93,
    KEY__KP_6 = 94,
    KEY__KP_7 = 95,
    KEY__KP_8 = 96,
    KEY__KP_9 = 97,
    KEY__KP_0 = 98,
    KEY__KP_PERIOD = 99,
 
    KEY__NONUSBACKSLASH = 100,
    KEY__APPLICATION = 101,
     KEY__KP_EQUALS = 103,
     KEY__F13 = 104,
     KEY__F14 = 105,
     KEY__F15 = 106,
     KEY__F16 = 107,
     KEY__F17 = 108,
     KEY__F18 = 109,
     KEY__F19 = 110,
     KEY__F20 = 111,
     KEY__F21 = 112,
     KEY__F22 = 113,
     KEY__F23 = 114,
     KEY__F24 = 115,
     KEY__EXECUTE = 116,
     KEY__HELP = 117,    
     KEY__MENU = 118,    
     KEY__SELECT = 119,
     KEY__STOP = 120,    
     KEY__AGAIN = 121,
     KEY__UNDO = 122,    
     KEY__CUT = 123,     
     KEY__COPY = 124,    
     KEY__PASTE = 125,   
     KEY__FIND = 126,    
     KEY__MUTE = 127,
     KEY__VOLUMEUP = 128,
     KEY__VOLUMEDOWN = 129,
     KEY__KP_COMMA = 133,
     KEY__KP_EQUALSAS400 = 134,
     
     KEY__INTERNATIONAL1 = 135,
     KEY__INTERNATIONAL2 = 136,
     KEY__INTERNATIONAL3 = 137,
     KEY__INTERNATIONAL4 = 138,
     KEY__INTERNATIONAL5 = 139,
     KEY__INTERNATIONAL6 = 140,
     KEY__INTERNATIONAL7 = 141,
     KEY__INTERNATIONAL8 = 142,
     KEY__INTERNATIONAL9 = 143,
     KEY__LANG1 = 144, 
     KEY__LANG2 = 145, 
     KEY__LANG3 = 146, 
     KEY__LANG4 = 147, 
     KEY__LANG5 = 148, 
     KEY__LANG6 = 149, 
     KEY__LANG7 = 150, 
     KEY__LANG8 = 151, 
     KEY__LANG9 = 152, 
 
     KEY__ALTERASE = 153,   
     KEY__SYSREQ = 154,
     KEY__CANCEL = 155,     
     KEY__CLEAR = 156,
     KEY__PRIOR = 157,
     KEY__RETURN2 = 158,
     KEY__SEPARATOR = 159,
     KEY__OUT = 160,
     KEY__OPER = 161,
     KEY__CLEARAGAIN = 162,
     KEY__CRSEL = 163,
     KEY__EXSEL = 164,
     
     KEY__KP_00 = 176,
     KEY__KP_000 = 177,
     KEY__THOUSANDSSEPARATOR = 178,
     KEY__DECIMALSEPARATOR = 179,
     KEY__CURRENCYUNIT = 180,
     KEY__CURRENCYSUBUNIT = 181,
     KEY__KP_LEFTPAREN = 182,
     KEY__KP_RIGHTPAREN = 183,
     KEY__KP_LEFTBRACE = 184,
     KEY__KP_RIGHTBRACE = 185,
     KEY__KP_TAB = 186,
     KEY__KP_BACKSPACE = 187,
     KEY__KP_A = 188,
     KEY__KP_B = 189,
     KEY__KP_C = 190,
     KEY__KP_D = 191,
     KEY__KP_E = 192,
     KEY__KP_F = 193,
     KEY__KP_XOR = 194,
     KEY__KP_POWER = 195,
     KEY__KP_PERCENT = 196,
     KEY__KP_LESS = 197,
     KEY__KP_GREATER = 198,
     KEY__KP_AMPERSAND = 199,
     KEY__KP_DBLAMPERSAND = 200,
     KEY__KP_VERTICALBAR = 201,
     KEY__KP_DBLVERTICALBAR = 202,
     KEY__KP_COLON = 203,
     KEY__KP_HASH = 204,
     KEY__KP_SPACE = 205,
     KEY__KP_AT = 206,
     KEY__KP_EXCLAM = 207,
     KEY__KP_MEMSTORE = 208,
     KEY__KP_MEMRECALL = 209,
     KEY__KP_MEMCLEAR = 210,
     KEY__KP_MEMADD = 211,
     KEY__KP_MEMSUBTRACT = 212,
     KEY__KP_MEMMULTIPLY = 213,
     KEY__KP_MEMDIVIDE = 214,
     KEY__KP_PLUSMINUS = 215,
     KEY__KP_CLEAR = 216,
     KEY__KP_CLEARENTRY = 217,
     KEY__KP_BINARY = 218,
     KEY__KP_OCTAL = 219,
     KEY__KP_DECIMAL = 220,
     KEY__KP_HEXADECIMAL = 221,
     
     KEY__LCTRL = 224,
     KEY__LSHIFT = 225,
     KEY__LALT = 226,
     KEY__LGUI = 227,
     KEY__RCTRL = 228,
     KEY__RSHIFT = 229,
     KEY__RALT = 230,
     KEY__RGUI = 231, 
     
     KEY__MODE = 257,   
     
     KEY__SLEEP = 258,                 
     KEY__WAKE = 259,                  
     
     KEY__CHANNEL_INCREMENT = 260,      
     KEY__CHANNEL_DECREMENT = 261,
     
     KEY__MEDIA_PLAY = 262,          
     KEY__MEDIA_PAUSE = 263,         
     KEY__MEDIA_RECORD = 264,        
     KEY__MEDIA_FAST_FORWARD = 265,   
     KEY__MEDIA_REWIND = 266,        
     KEY__MEDIA_NEXT_TRACK = 267,     
     KEY__MEDIA_PREVIOUS_TRACK = 268, 
     KEY__MEDIA_STOP = 269,          
     KEY__MEDIA_EJECT = 270,         
     KEY__MEDIA_PLAY_PAUSE = 271,     
     KEY__MEDIA_SELECT = 272,         
     
     KEY__AC_NEW = 273,              
     KEY__AC_OPEN = 274,             
     KEY__AC_CLOSE = 275,            
     KEY__AC_EXIT = 276,             
     KEY__AC_SAVE = 277,             
     KEY__AC_PRINT = 278,            
     KEY__AC_PROPERTIES = 279,       
     
     KEY__AC_SEARCH = 280,           
     KEY__AC_HOME = 281,             
     KEY__AC_BACK = 282,             
     KEY__AC_FORWARD = 283,          
     KEY__AC_STOP = 284,             
     KEY__AC_REFRESH = 285,          
     KEY__AC_BOOKMARKS = 286,        
     

     
     KEY__SOFTLEFT = 287,
     KEY__SOFTRIGHT = 288,
     KEY__CALL = 289, 
     KEY__ENDCALL = 290,
     
     
     KEY__RESERVED = 400,    
     
     KEY__COUNT = 512 
     
    };

    inline std::string Key_toString(KEY_CODE key) {
        switch (key) {
            case KEY_CODE::KEY__UNKNOWN: return "Unknown";
            case KEY_CODE::KEY__A: return "a";
            case KEY_CODE::KEY__B: return "b";
            case KEY_CODE::KEY__C: return "c";
            case KEY_CODE::KEY__D: return "d";
            case KEY_CODE::KEY__E: return "e";
            case KEY_CODE::KEY__F: return "f";
            case KEY_CODE::KEY__G: return "g";
            case KEY_CODE::KEY__H: return "h";
            case KEY_CODE::KEY__I: return "i";
            case KEY_CODE::KEY__J: return "j";
            case KEY_CODE::KEY__K: return "k";
            case KEY_CODE::KEY__L: return "l";
            case KEY_CODE::KEY__M: return "m";
            case KEY_CODE::KEY__N: return "n";
            case KEY_CODE::KEY__O: return "o";
            case KEY_CODE::KEY__P: return "p";
            case KEY_CODE::KEY__Q: return "q";
            case KEY_CODE::KEY__R: return "r";
            case KEY_CODE::KEY__S: return "s";
            case KEY_CODE::KEY__T: return "t";
            case KEY_CODE::KEY__U: return "u";
            case KEY_CODE::KEY__V: return "v";
            case KEY_CODE::KEY__W: return "w";
            case KEY_CODE::KEY__X: return "x";
            case KEY_CODE::KEY__Y: return "y";
            case KEY_CODE::KEY__Z: return "z";
            case KEY_CODE::KEY__1: return "1";
            case KEY_CODE::KEY__2: return "2";
            case KEY_CODE::KEY__3: return "3";
            case KEY_CODE::KEY__4: return "4";
            case KEY_CODE::KEY__5: return "5";
            case KEY_CODE::KEY__6: return "6";
            case KEY_CODE::KEY__7: return "7";
            case KEY_CODE::KEY__8: return "8";
            case KEY_CODE::KEY__9: return "9";
            case KEY_CODE::KEY__0: return "0";
            case KEY_CODE::KEY__RETURN: return "Return";
            case KEY_CODE::KEY__ESCAPE: return "Escape";
            case KEY_CODE::KEY__BACKSPACE: return "Backspace";
            case KEY_CODE::KEY__TAB: return "Tab";
            case KEY_CODE::KEY__SPACE: return "Space";
            case KEY_CODE::KEY__MINUS: return "-";
            case KEY_CODE::KEY__EQUALS: return "=";
            case KEY_CODE::KEY__LEFTBRACKET: return "[";
            case KEY_CODE::KEY__RIGHTBRACKET: return "]";
            case KEY_CODE::KEY__BACKSLASH: return "\\";
            case KEY_CODE::KEY__NONUSHASH: return "Non-US Hash";
            case KEY_CODE::KEY__SEMICOLON: return ";";
            case KEY_CODE::KEY__APOSTROPHE: return "'";
            case KEY_CODE::KEY__GRAVE: return "`";
            case KEY_CODE::KEY__COMMA: return ",";
            case KEY_CODE::KEY__PERIOD: return ".";
            case KEY_CODE::KEY__SLASH: return "/";
            case KEY_CODE::KEY__CAPSLOCK: return "Caps Lock";
            case KEY_CODE::KEY__F1: return "F1";
            case KEY_CODE::KEY__F2: return "F2";
            case KEY_CODE::KEY__F3: return "F3";
            case KEY_CODE::KEY__F4: return "F4";
            case KEY_CODE::KEY__F5: return "F5";
            case KEY_CODE::KEY__F6: return "F6";
            case KEY_CODE::KEY__F7: return "F7";
            case KEY_CODE::KEY__F8: return "F8";
            case KEY_CODE::KEY__F9: return "F9";
            case KEY_CODE::KEY__F10: return "F10";
            case KEY_CODE::KEY__F11: return "F11";
            case KEY_CODE::KEY__F12: return "F12";
            case KEY_CODE::KEY__PRINTSCREEN: return "Print Screen";
            case KEY_CODE::KEY__SCROLLLOCK: return "Scroll Lock";
            case KEY_CODE::KEY__PAUSE: return "Pause";
            case KEY_CODE::KEY__INSERT: return "Insert";
            case KEY_CODE::KEY__HOME: return "Home";
            case KEY_CODE::KEY__PAGEUP: return "Page Up";
            case KEY_CODE::KEY__DELETE: return "Delete";
            case KEY_CODE::KEY__END: return "End";
            case KEY_CODE::KEY__PAGEDOWN: return "Page Down";
            case KEY_CODE::KEY__RIGHT: return "Right";
            case KEY_CODE::KEY__LEFT: return "Left";
            case KEY_CODE::KEY__DOWN: return "Down";
            case KEY_CODE::KEY__UP: return "Up";
            case KEY_CODE::KEY__NUMLOCKCLEAR: return "Num Lock";
            case KEY_CODE::KEY__KP_DIVIDE: return "Keypad /";
            case KEY_CODE::KEY__KP_MULTIPLY: return "Keypad *";
            case KEY_CODE::KEY__KP_MINUS: return "Keypad -";
            case KEY_CODE::KEY__KP_PLUS: return "Keypad +";
            case KEY_CODE::KEY__KP_ENTER: return "Keypad Enter";
            case KEY_CODE::KEY__KP_1: return "Keypad 1";
            case KEY_CODE::KEY__KP_2: return "Keypad 2";
            case KEY_CODE::KEY__KP_3: return "Keypad 3";
            case KEY_CODE::KEY__KP_4: return "Keypad 4";
            case KEY_CODE::KEY__KP_5: return "Keypad 5";
            case KEY_CODE::KEY__KP_6: return "Keypad 6";
            case KEY_CODE::KEY__KP_7: return "Keypad 7";
            case KEY_CODE::KEY__KP_8: return "Keypad 8";
            case KEY_CODE::KEY__KP_9: return "Keypad 9";
            case KEY_CODE::KEY__KP_0: return "Keypad 0";
            case KEY_CODE::KEY__KP_PERIOD: return "Keypad .";
            case KEY_CODE::KEY__NONUSBACKSLASH: return "Non-US \\";
            case KEY_CODE::KEY__APPLICATION: return "Application";
            case KEY_CODE::KEY__KP_EQUALS: return "Keypad =";
            case KEY_CODE::KEY__F13: return "F13";
            case KEY_CODE::KEY__F14: return "F14";
            case KEY_CODE::KEY__F15: return "F15";
            case KEY_CODE::KEY__F16: return "F16";
            case KEY_CODE::KEY__F17: return "F17";
            case KEY_CODE::KEY__F18: return "F18";
            case KEY_CODE::KEY__F19: return "F19";
            case KEY_CODE::KEY__F20: return "F20";
            case KEY_CODE::KEY__F21: return "F21";
            case KEY_CODE::KEY__F22: return "F22";
            case KEY_CODE::KEY__F23: return "F23";
            case KEY_CODE::KEY__F24: return "F24";
            case KEY_CODE::KEY__EXECUTE: return "Execute";
            case KEY_CODE::KEY__HELP: return "Help";
            case KEY_CODE::KEY__MENU: return "Menu";
            case KEY_CODE::KEY__SELECT: return "Select";
            case KEY_CODE::KEY__STOP: return "Stop";
            case KEY_CODE::KEY__AGAIN: return "Again";
            case KEY_CODE::KEY__UNDO: return "Undo";
            case KEY_CODE::KEY__CUT: return "Cut";
            case KEY_CODE::KEY__COPY: return "Copy";
            case KEY_CODE::KEY__PASTE: return "Paste";
            case KEY_CODE::KEY__FIND: return "Find";
            case KEY_CODE::KEY__MUTE: return "Mute";
            case KEY_CODE::KEY__VOLUMEUP: return "Volume Up";
            case KEY_CODE::KEY__VOLUMEDOWN: return "Volume Down";
            case KEY_CODE::KEY__KP_COMMA: return "Keypad ,";
            case KEY_CODE::KEY__KP_EQUALSAS400: return "Keypad = (AS400)";
            case KEY_CODE::KEY__INTERNATIONAL1: return "International 1";
            case KEY_CODE::KEY__INTERNATIONAL2: return "International 2";
            case KEY_CODE::KEY__INTERNATIONAL3: return "International 3";
            case KEY_CODE::KEY__INTERNATIONAL4: return "International 4";
            case KEY_CODE::KEY__INTERNATIONAL5: return "International 5";
            case KEY_CODE::KEY__INTERNATIONAL6: return "International 6";
            case KEY_CODE::KEY__INTERNATIONAL7: return "International 7";
            case KEY_CODE::KEY__INTERNATIONAL8: return "International 8";
            case KEY_CODE::KEY__INTERNATIONAL9: return "International 9";
            case KEY_CODE::KEY__LANG1: return "Lang 1";
            case KEY_CODE::KEY__LANG2: return "Lang 2";
            case KEY_CODE::KEY__LANG3: return "Lang 3";
            case KEY_CODE::KEY__LANG4: return "Lang 4";
            case KEY_CODE::KEY__LANG5: return "Lang 5";
            case KEY_CODE::KEY__LANG6: return "Lang 6";
            case KEY_CODE::KEY__LANG7: return "Lang 7";
            case KEY_CODE::KEY__LANG8: return "Lang 8";
            case KEY_CODE::KEY__LANG9: return "Lang 9";
            case KEY_CODE::KEY__ALTERASE: return "Alt Erase";
            case KEY_CODE::KEY__SYSREQ: return "SysReq";
            case KEY_CODE::KEY__CANCEL: return "Cancel";
            case KEY_CODE::KEY__CLEAR: return "Clear";
            case KEY_CODE::KEY__PRIOR: return "Prior";
            case KEY_CODE::KEY__RETURN2: return "Return";
            case KEY_CODE::KEY__SEPARATOR: return "Separator";
            case KEY_CODE::KEY__OUT: return "Out";
            case KEY_CODE::KEY__OPER: return "Oper";
            case KEY_CODE::KEY__CLEARAGAIN: return "Clear Again";
            case KEY_CODE::KEY__CRSEL: return "CrSel";
            case KEY_CODE::KEY__EXSEL: return "ExSel";
            case KEY_CODE::KEY__KP_00: return "Keypad 00";
            case KEY_CODE::KEY__KP_000: return "Keypad 000";
            case KEY_CODE::KEY__THOUSANDSSEPARATOR: return "Thousands Separator";
            case KEY_CODE::KEY__DECIMALSEPARATOR: return "Decimal Separator";
            case KEY_CODE::KEY__CURRENCYUNIT: return "Currency Unit";
            case KEY_CODE::KEY__CURRENCYSUBUNIT: return "Currency Subunit";
            case KEY_CODE::KEY__KP_LEFTPAREN: return "Keypad (";
            case KEY_CODE::KEY__KP_RIGHTPAREN: return "Keypad )";
            case KEY_CODE::KEY__KP_LEFTBRACE: return "Keypad {";
            case KEY_CODE::KEY__KP_RIGHTBRACE: return "Keypad }";
            case KEY_CODE::KEY__KP_TAB: return "Keypad Tab";
            case KEY_CODE::KEY__KP_BACKSPACE: return "Keypad Backspace";
            case KEY_CODE::KEY__KP_A: return "Keypad A";
            case KEY_CODE::KEY__KP_B: return "Keypad B";
            case KEY_CODE::KEY__KP_C: return "Keypad C";
            case KEY_CODE::KEY__KP_D: return "Keypad D";
            case KEY_CODE::KEY__KP_E: return "Keypad E";
            case KEY_CODE::KEY__KP_F: return "Keypad F";
            case KEY_CODE::KEY__KP_XOR: return "Keypad XOR";
            case KEY_CODE::KEY__KP_POWER: return "Keypad Power";
            case KEY_CODE::KEY__KP_PERCENT: return "Keypad %";
            case KEY_CODE::KEY__KP_LESS: return "Keypad <";
            case KEY_CODE::KEY__KP_GREATER: return "Keypad >";
            case KEY_CODE::KEY__KP_AMPERSAND: return "Keypad &";
            case KEY_CODE::KEY__KP_DBLAMPERSAND: return "Keypad &&";
            case KEY_CODE::KEY__KP_VERTICALBAR: return "Keypad |";
            case KEY_CODE::KEY__KP_DBLVERTICALBAR: return "Keypad ||";
            case KEY_CODE::KEY__KP_COLON: return "Keypad :";
            case KEY_CODE::KEY__KP_HASH: return "Keypad #";
            case KEY_CODE::KEY__KP_SPACE: return "Keypad Space";
            case KEY_CODE::KEY__KP_AT: return "Keypad @";
            case KEY_CODE::KEY__KP_EXCLAM: return "Keypad !";
            case KEY_CODE::KEY__KP_MEMSTORE: return "Keypad Mem Store";
            case KEY_CODE::KEY__KP_MEMRECALL: return "Keypad Mem Recall";
            case KEY_CODE::KEY__KP_MEMCLEAR: return "Keypad Mem Clear";
            case KEY_CODE::KEY__KP_MEMADD: return "Keypad Mem Add";
            case KEY_CODE::KEY__KP_MEMSUBTRACT: return "Keypad Mem Subtract";
            case KEY_CODE::KEY__KP_MEMMULTIPLY: return "Keypad Mem Multiply";
            case KEY_CODE::KEY__KP_MEMDIVIDE: return "Keypad Mem Divide";
            case KEY_CODE::KEY__KP_PLUSMINUS: return "Keypad +/-";
            case KEY_CODE::KEY__KP_CLEAR: return "Keypad Clear";
            case KEY_CODE::KEY__KP_CLEARENTRY: return "Keypad Clear Entry";
            case KEY_CODE::KEY__KP_BINARY: return "Keypad Binary";
            case KEY_CODE::KEY__KP_OCTAL: return "Keypad Octal";
            case KEY_CODE::KEY__KP_DECIMAL: return "Keypad Decimal";
            case KEY_CODE::KEY__KP_HEXADECIMAL: return "Keypad Hexadecimal";
            case KEY_CODE::KEY__LCTRL: return "Left Control";
            case KEY_CODE::KEY__LSHIFT: return "Left Shift";
            case KEY_CODE::KEY__LALT: return "Left Alt";
            case KEY_CODE::KEY__LGUI: return "Left GUI";
            case KEY_CODE::KEY__RCTRL: return "Right Control";
            case KEY_CODE::KEY__RSHIFT: return "Right Shift";
            case KEY_CODE::KEY__RALT: return "Right Alt";
            case KEY_CODE::KEY__RGUI: return "Right GUI";
            case KEY_CODE::KEY__MODE: return "Mode";
            case KEY_CODE::KEY__SLEEP: return "Sleep";
            case KEY_CODE::KEY__WAKE: return "Wake";
            case KEY_CODE::KEY__CHANNEL_INCREMENT: return "Channel Increment";
            case KEY_CODE::KEY__CHANNEL_DECREMENT: return "Channel Decrement";
            case KEY_CODE::KEY__MEDIA_PLAY: return "Media Play";
            case KEY_CODE::KEY__MEDIA_PAUSE: return "Media Pause";
            case KEY_CODE::KEY__MEDIA_RECORD: return "Media Record";
            case KEY_CODE::KEY__MEDIA_FAST_FORWARD: return "Media Fast Forward";
            case KEY_CODE::KEY__MEDIA_REWIND: return "Media Rewind";
            case KEY_CODE::KEY__MEDIA_NEXT_TRACK: return "Media Next Track";
            case KEY_CODE::KEY__MEDIA_PREVIOUS_TRACK: return "Media Previous Track";
            case KEY_CODE::KEY__MEDIA_STOP: return "Media Stop";
            case KEY_CODE::KEY__MEDIA_EJECT: return "Media Eject";
            case KEY_CODE::KEY__MEDIA_PLAY_PAUSE: return "Media Play/Pause";
            case KEY_CODE::KEY__MEDIA_SELECT: return "Media Select";
            case KEY_CODE::KEY__AC_NEW: return "AC New";
            case KEY_CODE::KEY__AC_OPEN: return "AC Open";
            case KEY_CODE::KEY__AC_CLOSE: return "AC Close";
            case KEY_CODE::KEY__AC_EXIT: return "AC Exit";
            case KEY_CODE::KEY__AC_SAVE: return "AC Save";
            case KEY_CODE::KEY__AC_PRINT: return "AC Print";
            case KEY_CODE::KEY__AC_PROPERTIES: return "AC Properties";
            case KEY_CODE::KEY__AC_SEARCH: return "AC Search";
            case KEY_CODE::KEY__AC_HOME: return "AC Home";
            case KEY_CODE::KEY__AC_BACK: return "AC Back";
            case KEY_CODE::KEY__AC_FORWARD: return "AC Forward";
            case KEY_CODE::KEY__AC_STOP: return "AC Stop";
            case KEY_CODE::KEY__AC_REFRESH: return "AC Refresh";
            case KEY_CODE::KEY__AC_BOOKMARKS: return "AC Bookmarks";
            case KEY_CODE::KEY__SOFTLEFT: return "Soft Left";
            case KEY_CODE::KEY__SOFTRIGHT: return "Soft Right";
            case KEY_CODE::KEY__CALL: return "Call";
            case KEY_CODE::KEY__ENDCALL: return "End Call";
            case KEY_CODE::KEY__COUNT: return "512";
            default: return "Unknown";
        }
    }

    inline std::string Button_toString(MOUSE_CODE button) {
        switch(button) {
            case MOUSE_CODE::ButtonLeft: // Mouse 0
                return "Left Button";
                break;
            case MOUSE_CODE::ButtonRight: // Mouse 3
                return "Right Button";
                break;
            case MOUSE_CODE::ButtonMiddle: // Mouse 2
                return "Middle Button";
                break;
            case MOUSE_CODE::Button1:
                return "Button 1";
                break;
            case MOUSE_CODE::Button4:
                return "Button 4";
                break;
            case MOUSE_CODE::Button5:
                return "Button 5";
                break;
            case MOUSE_CODE::Button6:
                return "Button 6";
                break;
            case MOUSE_CODE::Button7:
                return "Button 7";
                break;
            default:
                return "Unknown button!";
        }
    }

}   
}


using KeyCode = Sleak::Input::KEY_CODE;
using MouseCode = Sleak::Input::MOUSE_CODE;


 #endif