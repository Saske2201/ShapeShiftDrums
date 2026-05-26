#define PLUG_NAME "ShapeShiftDrums"
#define PLUG_MFR "AquamarineRecords"
#define PLUG_VERSION_HEX 0x00000000
#define PLUG_VERSION_STR "0.0.0"
#define PLUG_UNIQUE_ID '2201'      // << проверьте уникальность в ваших проектах
#define PLUG_MFR_ID 'AQMR'         
#define PLUG_URL_STR "https://iplug2.github.io"         
#define PLUG_EMAIL_STR "info@aquamarinerecords.com"     
#define PLUG_COPYRIGHT_STR "Copyright 2025 Aquamarine Records"
#define PLUG_CLASS_NAME TemplateProject

#define BUNDLE_NAME "TemplateProject"
#define BUNDLE_MFR "AquamarineRecords"
#define BUNDLE_DOMAIN "com"

#define SHARED_RESOURCES_SUBPATH "TemplateProject"

// Вместо единого #define PLUG_CHANNEL_IO ...:
#if defined(APP_API)
#define PLUG_CHANNEL_IO "2-2"
#else
#define PLUG_CHANNEL_IO " \
0-2 \
0-2.2 \
0-2.2.2 \
0-2.2.2.2 \
0-2.2.2.2.2 \
0-2.2.2.2.2.2 \
0-2.2.2.2.2.2.2"
#endif

#define PLUG_LATENCY 0
#define PLUG_TYPE 1                 // Instrument
#define PLUG_DOES_MIDI_IN 1
#define PLUG_DOES_MIDI_OUT 1
#define PLUG_DOES_MPE 1
#define PLUG_DOES_STATE_CHUNKS 0
#define PLUG_HAS_UI 1
#define PLUG_WIDTH 1500
#define PLUG_HEIGHT 864
#define PLUG_FPS 60
#define PLUG_SHARED_RESOURCES 1
#define PLUG_HOST_RESIZE 0
#define PLUG_MIN_WIDTH 1500
#define PLUG_MIN_HEIGHT 864

// AUv2 идентификаторы из класса TemplateProject
#define AUV2_ENTRY TemplateProject_Entry
#define AUV2_ENTRY_STR "TemplateProject_Entry"
#define AUV2_FACTORY TemplateProject_Factory
#define AUV2_VIEW_CLASS TemplateProject_View
#define AUV2_VIEW_CLASS_STR "TemplateProject_View"

#define AAX_TYPE_IDS 'ITP1'
#define AAX_TYPE_IDS_AUDIOSUITE 'ITA1'
#define AAX_PLUG_MFR_STR "Aquamarine"
#define AAX_PLUG_NAME_STR "TemplateProject\nIPEF"
#define AAX_PLUG_CATEGORY_STR "Drum"   
#define AAX_DOES_AUDIOSUITE 1

#define VST3_SUBCATEGORY "Instrument"    

#define CLAP_MANUAL_URL "https://iplug2.github.io/manuals/example_manual.pdf"
#define CLAP_SUPPORT_URL "https://github.com/iPlug2/iPlug2/wiki"
#define CLAP_DESCRIPTION "A drum instrument with multi-outs"
#define CLAP_FEATURES "instrument" 

#define APP_NUM_CHANNELS 2
#define APP_N_VECTOR_WAIT 0
#define APP_MULT 1
#define APP_COPY_AUV3 0
#define APP_SIGNAL_VECTOR_SIZE 64

#define ROBOTO_FN "Roboto-Regular.ttf"
#define IMPACT_FN "impact.ttf"

#define IMG_BACKGROUND_FN "Background.png"
#define IMG_FOREGROUND_FN "Drum_Foreground.png"
#define IMG_PUNCH_FN "Focus.png" 
#define IMG_MENU_BUTTON_BG_FN "Button_BG.png" 
#define IMG_MENU_MAPPING_BG_FN "Mapping_BG.png" 
#define IMG_MENU_MIXER_BG_FN "Mixer_01_BG.png" 
#define IMG_MENU_MIXER_BG_UND_FN "Mixer_01_BG_under.png" 
#define IMG_MENU_ABOUT_BG_FN "About_BG.png" 

#define IMG_MENU_CYMBALS_BG_FN "Mixer_01_cymbals_BG.png" 
#define IMG_MENU_CYMBALS_BG_UND_FN "Mixer_01_cymbals_BG_under.png" 

#define IMG_MENU_ROOMS_BG_FN "Mixer_01_rooms_BG.png" 


#define IMG_SLIDER_HANDLE_FN "IBSlider_Handle.png" 
#define IMG_SLIDER_HANDLE_01_FN "IBSlider_Handle_01.png" 
#define IMG_SLIDER_HANDLE_02_FN "IBSlider_Handle_02.png" 
#define IMG_SLIDER_HANDLE_03_FN "IBSlider_Handle_03.png" 
#define IMG_SLIDER_HANDLE_04_FN "IBSlider_Handle_04.png" 
#define IMG_SLIDER_HANDLE_05_FN "IBSlider_Handle_05.png" 

#define IMG_KNOB_BODY_FN "IKnob_Body.png" 
#define IMG_KNOB_POINTER_FN "IKnob_Pointer.png" 

#define IMG_KICK_OVERLAY_FN "Kick_Overlay.png"
#define IMG_SNARE_OVERLAY_FN "Snare_Overlay.png"
#define IMG_CRASH_L_OVERLAY_FN "Crash_L_Overlay.png"
#define IMG_CRASH_R_OVERLAY_FN "Crash_R_Overlay.png"
#define IMG_TOM_3_OVERLAY_FN "Tom_Floor_Overlay.png"
#define IMG_TOM_2_OVERLAY_FN "Tom_Rack_Right_Overlay.png"
#define IMG_TOM_1_OVERLAY_FN "Tom_Rack_Left_Overlay.png"
#define IMG_SPLASH_OVERLAY_FN "Splash_Overlay.png"
#define IMG_RIDE_OVERLAY_FN "Ride_Overlay.png"
#define IMG_HH_OVERLAY_FN "HH_Overlay.png"
#define IMG_CHINA_OVERLAY_FN "China_Overlay.png"

#define IMG_BUTTON_SOLO_01_ON_FN "Button_Solo_01_on.png"
#define IMG_BUTTON_SOLO_01_OFF_FN "Button_Solo_01_off.png"
#define IMG_BUTTON_SOLO_01_OFF_LIGHT_FN "Button_Solo_01_off_light.png"

#define IMG_BUTTON_MUTE_01_ON_FN "Button_Mute_01_on.png"
#define IMG_BUTTON_MUTE_01_OFF_FN "Button_Mute_01_off.png"
#define IMG_BUTTON_MUTE_01_OFF_LIGHT_FN "Button_Mute_01_off_light.png"

#define IMG_BUTTON_SOLO_02_ON_FN "Button_Solo_02_on.png"
#define IMG_BUTTON_SOLO_02_OFF_FN "Button_Solo_02_off.png"
#define IMG_BUTTON_SOLO_02_OFF_LIGHT_FN "Button_Solo_02_off_light.png"

#define IMG_BUTTON_MUTE_02_ON_FN "Button_Mute_02_on.png"
#define IMG_BUTTON_MUTE_02_OFF_FN "Button_Mute_02_off.png"
#define IMG_BUTTON_MUTE_02_OFF_LIGHT_FN "Button_Mute_02_off_light.png"

#define IMG_BUTTON_SOLO_03_ON_FN "Button_Solo_03_on.png"
#define IMG_BUTTON_SOLO_03_OFF_FN "Button_Solo_03_off.png"
#define IMG_BUTTON_SOLO_03_OFF_LIGHT_FN "Button_Solo_03_off_light.png"

#define IMG_BUTTON_MUTE_03_ON_FN "Button_Mute_03_on.png"
#define IMG_BUTTON_MUTE_03_OFF_FN "Button_Mute_03_off.png"
#define IMG_BUTTON_MUTE_03_OFF_LIGHT_FN "Button_Mute_03_off_light.png"

#define IMG_BUTTON_SOLO_04_ON_FN "Button_Solo_04_on.png"
#define IMG_BUTTON_SOLO_04_OFF_FN "Button_Solo_04_off.png"
#define IMG_BUTTON_SOLO_04_OFF_LIGHT_FN "Button_Solo_04_off_light.png"

#define IMG_BUTTON_MUTE_04_ON_FN "Button_Mute_04_on.png"
#define IMG_BUTTON_MUTE_04_OFF_FN "Button_Mute_04_off.png"
#define IMG_BUTTON_MUTE_04_OFF_LIGHT_FN "Button_Mute_04_off_light.png"

#define IMG_BROWSE_BG_FN "Browse_BG.png"

