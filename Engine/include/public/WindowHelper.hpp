#ifndef _WINDOWHELPER_HPP_
#define _WINDOWHELPER_HPP_

#include <Logger.hpp>
#include <SDL3/SDL.h>
#include <iostream>

namespace Sleak {
  
    enum MessageBoxType : unsigned int {
        Info = 0x00000040u,
        Warning = 0x00000020u,
        Error = 0x00000010u
    };

    enum MessageBoxReturn {
        Ok = 0,
        Yes = 1,
        No = 2,
        Cancel = 3
    };

    struct MessageBoxColorScheme 
    {
        Uint8 Background[3] = { 255,   0,   0 }; // Background
        Uint8 Text[3] = { 255, 255, 255 }; // Text
        Uint8 ButtonBorder[3] = {   0, 255,   0 }; // ButtonBorder
        Uint8 ButtonBackground[3] = {   0,   0, 255 }; // ButtonBackground
        Uint8 ButtonText[3] = { 255, 255,   0 }; // ButtonText   
    };

    void MessageBox(const char* Title, const char* Message, MessageBoxType Type) {
        if(SDL_WasInit(0) == 0)
        {
            SLEAK_WARN("SDL is not initialized yet! Cannot display message box.");
            return;
        }
        
        if(!SDL_ShowSimpleMessageBox(Type,Title,Message,nullptr))
        {
            SLEAK_ERROR("Failed to create message box! {}",SDL_GetError());
        }
    }

    MessageBoxReturn ShowMessageBox(const char* Title, const char* Message, MessageBoxType Type, MessageBoxColorScheme scheme = MessageBoxColorScheme()) {
        if(SDL_WasInit(0) == 0)
        {
            SLEAK_WARN("SDL is not initialized yet! Cannot display message box.");
            return MessageBoxReturn::Cancel;
        }

        SDL_MessageBoxColorScheme Colorscheme;
        Colorscheme.colors[0].r = scheme.Background[0];
        Colorscheme.colors[0].g = scheme.Background[1];
        Colorscheme.colors[0].b = scheme.Background[2];
    
        Colorscheme.colors[1].r = scheme.Text[0];
        Colorscheme.colors[1].g = scheme.Text[1];
        Colorscheme.colors[1].b = scheme.Text[2];
    
        Colorscheme.colors[2].r = scheme.ButtonBorder[0];
        Colorscheme.colors[2].g = scheme.ButtonBorder[1];
        Colorscheme.colors[2].b = scheme.ButtonBorder[2];
    
        Colorscheme.colors[3].r = scheme.ButtonBackground[0];
        Colorscheme.colors[3].g = scheme.ButtonBackground[1];
        Colorscheme.colors[3].b = scheme.ButtonBackground[2];
    
        Colorscheme.colors[4].r = scheme.ButtonText[0];
        Colorscheme.colors[4].g = scheme.ButtonText[1];
        Colorscheme.colors[4].b = scheme.ButtonText[2];

        const SDL_MessageBoxButtonData Buttons[] = {
            {0, MessageBoxReturn::Yes, "Yes"},  // First button
            {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, MessageBoxReturn::No, "No"}, 
            {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, MessageBoxReturn::Cancel, "Cancel"} 
        };

        SDL_MessageBoxData data;
        data.flags = (SDL_MessageBoxButtonFlags)Type;
        data.title = Title;
        data.message = Message;
        data.window = nullptr;
        data.buttons = Buttons;
        data.numbuttons = SDL_arraysize(Buttons);
        data.colorScheme = &Colorscheme;
        
        int button = 0;

        if(!SDL_ShowMessageBox(&data,&button))
        {
            SLEAK_ERROR("Failed to create message box! {}",SDL_GetError());
        }

        return (MessageBoxReturn)button;
    }
  

};
#endif