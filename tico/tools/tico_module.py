#!/usr/bin/env python3
"""Settings definition and translations for the tico module of Genesis Plus GX.

Run after merging upstream. It builds tools/dump_core_options.c against the
core's own libretro_core_options.h, so tico/module/settings.json lists exactly
the options the libnx core reads (genesis_plus_gx_* keys, values and defaults), laid
out in tabs, followed by the overlay's own display and controls options.
Labels are translation keys; the strings go into tico/lang/*.json, taken from
tico's existing settings labels where an option already had one and otherwise
from the core's own translations. Choice labels stay English in settings.json;
the overlay translates them through settings_genesis_plus_gx_value_* keys.

    python3 tico/tools/tico_module.py
"""

from __future__ import annotations

import json
import os
import re
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TICO = ROOT / "tico"
SETTINGS = TICO / "module/settings.json"
LANG_DIR = TICO / "lang"
LANGUAGES = ("en", "de", "es", "fr", "ja", "pt", "ru", "zh")
# tico-nx's own strings label the options it already knew; optional
TICO_NX = Path(os.environ.get("TICO_NX_DIR", ROOT.parents[1] / "tico-nx"))

# Core option -> label key. Options tico already labelled keep their key (and
# tico's translations); the rest are named after the option.
LABEL_KEYS = {
    "genesis_plus_gx_system_hw": "settings_genesis_plus_gx_hardware_model",
    "genesis_plus_gx_region_detect": "settings_genesis_plus_gx_region",
    "genesis_plus_gx_vdp_mode": "settings_genesis_plus_gx_vdp_mode",
    "genesis_plus_gx_bios": "settings_genesis_plus_gx_use_bios",
    "genesis_plus_gx_system_bram": "settings_genesis_plus_gx_system_bram",
    "genesis_plus_gx_cart_size": "settings_genesis_plus_gx_backup_ram_size",
    "genesis_plus_gx_cart_bram": "settings_genesis_plus_gx_cart_backup_ram",
    "genesis_plus_gx_lock_on": "settings_genesis_plus_gx_lock_on_cartridge",
    "genesis_plus_gx_add_on": "settings_genesis_plus_gx_expansion_port",
    "genesis_plus_gx_aspect_ratio": "settings_genesis_plus_gx_aspect_ratio",
    "genesis_plus_gx_overscan": "settings_genesis_plus_gx_overscan_borders",
    "genesis_plus_gx_left_border": "settings_genesis_plus_gx_left_border",
    "genesis_plus_gx_render": "settings_genesis_plus_gx_interlaced_mode",
    "genesis_plus_gx_blargg_ntsc_filter": "settings_genesis_plus_gx_ntsc_filter",
    "genesis_plus_gx_lcd_filter": "settings_genesis_plus_gx_lcd_ghosting",
    "genesis_plus_gx_gg_extra": "settings_genesis_plus_gx_game_gear_extended",
    "genesis_plus_gx_no_sprite_limit": "settings_genesis_plus_gx_remove_sprite_limit",
    "genesis_plus_gx_enhanced_vscroll": "settings_genesis_plus_gx_enhanced_vscroll",
    "genesis_plus_gx_enhanced_vscroll_limit": "settings_genesis_plus_gx_vscroll_limit",
    "genesis_plus_gx_frameskip": "settings_genesis_plus_gx_frameskip",
    "genesis_plus_gx_frameskip_threshold": "settings_genesis_plus_gx_frameskip_threshold_pct",
    "genesis_plus_gx_ym2612": "settings_genesis_plus_gx_ym2612_fm_core",
    "genesis_plus_gx_ym2413": "settings_genesis_plus_gx_ym2413_fm",
    "genesis_plus_gx_ym2413_core": "settings_genesis_plus_gx_ym2413_core",
    "genesis_plus_gx_sound_output": "settings_genesis_plus_gx_sound_output",
    "genesis_plus_gx_audio_filter": "settings_genesis_plus_gx_low_pass_filter",
    "genesis_plus_gx_lowpass_range": "settings_genesis_plus_gx_low_pass_range_pct",
    "genesis_plus_gx_overclock": "settings_genesis_plus_gx_cpu_overclock",
    "genesis_plus_gx_force_dtack": "settings_genesis_plus_gx_force_dtack",
    "genesis_plus_gx_addr_error": "settings_genesis_plus_gx_68k_address_error",
    "genesis_plus_gx_cd_latency": "settings_genesis_plus_gx_cd_access_latency",
    "genesis_plus_gx_cd_precache": "settings_genesis_plus_gx_cd_precache",
}

# Core options the frontend has no use for: light guns and mice need pointer
# input it does not send, and show_advanced_audio_settings only hides entries
# in RetroArch's menu.
EXCLUDED = {
    "genesis_plus_gx_gun_input", "genesis_plus_gx_gun_cursor", "genesis_plus_gx_invert_mouse",
    "genesis_plus_gx_show_advanced_audio_settings",
}

# (tab, [(section, [option keys])]). Every core option the dump lists must be
# placed or excluded; the overlay's own tabs are added around these.
LAYOUT = [
    ("settings_genesis_plus_gx_tab_system", [
        ("settings_genesis_plus_gx_section_console", [
            "genesis_plus_gx_system_hw", "genesis_plus_gx_region_detect",
            "genesis_plus_gx_vdp_mode", "genesis_plus_gx_bios",
        ]),
        ("settings_genesis_plus_gx_section_storage", [
            "genesis_plus_gx_system_bram", "genesis_plus_gx_cart_size", "genesis_plus_gx_cart_bram",
        ]),
        ("settings_genesis_plus_gx_section_expansions", [
            "genesis_plus_gx_lock_on", "genesis_plus_gx_add_on",
        ]),
    ]),
    ("settings_genesis_plus_gx_tab_video", [
        ("settings_genesis_plus_gx_section_display", [
            "genesis_plus_gx_aspect_ratio", "genesis_plus_gx_overscan",
            "genesis_plus_gx_left_border", "genesis_plus_gx_render",
        ]),
        ("settings_genesis_plus_gx_section_filters", [
            "genesis_plus_gx_blargg_ntsc_filter", "genesis_plus_gx_lcd_filter",
        ]),
        ("settings_genesis_plus_gx_section_enhancements", [
            "genesis_plus_gx_gg_extra", "genesis_plus_gx_no_sprite_limit",
            "genesis_plus_gx_enhanced_vscroll", "genesis_plus_gx_enhanced_vscroll_limit",
            "genesis_plus_gx_frameskip", "genesis_plus_gx_frameskip_threshold",
        ]),
    ]),
    ("settings_genesis_plus_gx_tab_audio", [
        ("settings_genesis_plus_gx_section_sound_chips", [
            "genesis_plus_gx_ym2612", "genesis_plus_gx_ym2413", "genesis_plus_gx_ym2413_core",
            "genesis_plus_gx_sound_output",
        ]),
        ("settings_genesis_plus_gx_section_filters", [
            "genesis_plus_gx_audio_filter", "genesis_plus_gx_lowpass_range",
            "genesis_plus_gx_audio_eq_low", "genesis_plus_gx_audio_eq_mid",
            "genesis_plus_gx_audio_eq_high",
        ]),
        ("settings_genesis_plus_gx_section_volume", [
            "genesis_plus_gx_psg_preamp", "genesis_plus_gx_fm_preamp",
            "genesis_plus_gx_cdda_volume", "genesis_plus_gx_pcm_volume",
        ]),
    ]),
    ("settings_genesis_plus_gx_tab_emulation", [
        ("settings_genesis_plus_gx_section_cpu", [
            "genesis_plus_gx_overclock", "genesis_plus_gx_force_dtack",
            "genesis_plus_gx_addr_error",
        ]),
        ("settings_genesis_plus_gx_section_cd", [
            "genesis_plus_gx_cd_latency", "genesis_plus_gx_cd_precache",
        ]),
    ]),
    ("settings_genesis_plus_gx_tab_advanced", [
        ("settings_genesis_plus_gx_section_sound_channels", [
            f"genesis_plus_gx_psg_channel_{i}_volume" for i in range(4)
        ] + [
            f"genesis_plus_gx_md_channel_{i}_volume" for i in range(6)
        ] + [
            f"genesis_plus_gx_sms_fm_channel_{i}_volume" for i in range(9)
        ]),
    ]),
]

# Listed only while another option has a value, like the core's own menus.
DEPENDS_ON = {
    "genesis_plus_gx_lowpass_range": ("genesis_plus_gx_audio_filter", "low-pass"),
    "genesis_plus_gx_audio_eq_low": ("genesis_plus_gx_audio_filter", "EQ"),
    "genesis_plus_gx_audio_eq_mid": ("genesis_plus_gx_audio_filter", "EQ"),
    "genesis_plus_gx_audio_eq_high": ("genesis_plus_gx_audio_filter", "EQ"),
    "genesis_plus_gx_enhanced_vscroll_limit": ("genesis_plus_gx_enhanced_vscroll", "enabled"),
    "genesis_plus_gx_frameskip_threshold": ("genesis_plus_gx_frameskip", "manual"),
}

# Switch buttons a Mega Drive button (or the fast-forward hotkey) can sit on.
SWITCH_BUTTONS = [("A", "A"), ("B", "B"), ("X", "X"), ("Y", "Y"), ("L", "L"), ("R", "R"),
                  ("ZL", "ZL"), ("ZR", "ZR"), ("Plus", "Plus"), ("Minus", "Minus"),
                  ("StickL", "Left stick"), ("StickR", "Right stick"), ("Up", "Up"),
                  ("Down", "Down"), ("Left", "Left"), ("Right", "Right"), ("None", "Disabled")]

POSITIONS = [("hidden", "Hidden"), ("top_left", "Top left"), ("top_right", "Top right"),
             ("bottom_left", "Bottom left"), ("bottom_right", "Bottom right")]

# The overlay's own options: how the game is scaled, fast forward and the HUD.
# Shaders are picked in game (Settings > Shaders): tico cannot see the presets
# on the SD card.
OVERLAY_TAB = ("settings_genesis_plus_gx_tab_display", [
    ("settings_genesis_plus_gx_section_screen", [
        {"key": "display_mode", "label": "settings_genesis_plus_gx_display_mode", "type": "enum",
         "default": "Display", "choices": [("Integer", "Integer"), ("Display", "Display")]},
        {"key": "display_size", "label": "settings_genesis_plus_gx_display_size", "type": "enum",
         "default": "4:3", "choices": [("Stretch", "Stretch"), ("4:3", "4:3"), ("16:9", "16:9"),
                                             ("Original", "Original"), ("1x", "1x"), ("2x", "2x"),
                                             ("Auto", "Auto")]},
    ]),
    ("settings_genesis_plus_gx_section_fast_forward", [
        {"key": "fast_forward_speed", "label": "settings_genesis_plus_gx_fast_forward_speed", "type": "enum",
         "default": "200", "choices": [("150", "150%"), ("200", "200%"), ("300", "300%"),
                                        ("400", "400%"), ("unlimited", "Unlimited")]},
        {"key": "fast_forward_mode", "label": "settings_genesis_plus_gx_fast_forward_mode", "type": "enum",
         "default": "hold", "choices": [("hold", "Hold"), ("toggle", "Toggle")]},
        {"key": "fast_forward_hotkey", "label": "settings_genesis_plus_gx_fast_forward_hotkey", "type": "enum",
         "default": "ZR", "choices": SWITCH_BUTTONS},
    ]),
    ("settings_genesis_plus_gx_section_hud", [
        {"key": "fps_counter_position", "label": "settings_genesis_plus_gx_fps_counter", "type": "enum",
         "default": "hidden", "choices": POSITIONS},
        {"key": "rendered_ir_position", "label": "settings_genesis_plus_gx_rendered_resolution",
         "type": "enum", "default": "hidden", "choices": POSITIONS},
    ]),
])

# The overlay's button mapping: Mega Drive button -> Switch button, by default
# where RetroArch puts the core's six-button pad (A, B, C on Y, B, A).
CONTROLS_TAB = ("settings_genesis_plus_gx_tab_controls", [
    ("settings_genesis_plus_gx_section_button_mapping", [
        {"key": key, "label": "settings_genesis_plus_gx_" + key, "type": "enum", "default": default,
         "choices": SWITCH_BUTTONS}
        for key, default in [
            ("map_a", "Y"), ("map_b", "B"), ("map_c", "A"), ("map_x", "L"), ("map_y", "X"),
            ("map_z", "R"), ("map_start", "Plus"), ("map_mode", "Minus"),
            ("map_up", "Up"), ("map_down", "Down"), ("map_left", "Left"), ("map_right", "Right"),
        ]
    ] + [
        {"key": "analog_dpad", "label": "settings_genesis_plus_gx_analog_dpad", "type": "bool",
         "default": "enabled"},
    ]),
])

# Labels the core does not define. key -> (en, de, es, fr, ja, pt, ru, zh)
LABELS = {
    "settings_genesis_plus_gx_tab_display": ("Display", "Anzeige", "Pantalla", "Affichage", "表示", "Tela",
                                    "Экран", "显示"),
    "settings_genesis_plus_gx_tab_advanced": ("Advanced", "Erweitert", "Avanzado", "Avancé", "詳細",
                                     "Avançado", "Дополнительно", "高级"),
    "settings_genesis_plus_gx_section_volume": ("Volume", "Lautstärke", "Volumen", "Volume", "音量",
                                                "Volume", "Громкость", "音量"),
    "settings_genesis_plus_gx_section_sound_channels": ("Sound channels", "Tonkanäle", "Canales de sonido",
                                               "Canaux sonores", "サウンドチャンネル",
                                               "Canais de som", "Звуковые каналы", "声道"),
    "settings_genesis_plus_gx_section_screen": ("Screen", "Bild", "Imagen", "Image", "画面", "Imagem",
                                       "Изображение", "画面"),
    "settings_genesis_plus_gx_section_hud": ("On-screen info", "Bildschirmanzeige", "Información en pantalla",
                                    "Affichage à l'écran", "画面表示", "Informações na tela",
                                    "Экранная информация", "屏幕信息"),
    "settings_genesis_plus_gx_display_mode": ("Display Mode", "Anzeigemodus", "Modo de pantalla",
                                     "Mode d'affichage", "表示モード", "Modo de exibição",
                                     "Режим отображения", "显示模式"),
    "settings_genesis_plus_gx_display_size": ("Size", "Größe", "Tamaño", "Taille", "サイズ", "Tamanho",
                                     "Размер", "尺寸"),
    "settings_genesis_plus_gx_fps_counter": ("FPS counter", "FPS-Zähler", "Contador de FPS",
                                    "Compteur de FPS", "FPSカウンター", "Contador de FPS",
                                    "Счётчик FPS", "帧率计数器"),
    "settings_genesis_plus_gx_rendered_resolution": ("Rendered resolution", "Gerenderte Auflösung",
                                            "Resolución renderizada", "Résolution de rendu",
                                            "描画解像度", "Resolução renderizada",
                                            "Разрешение рендеринга", "渲染分辨率"),
    "settings_genesis_plus_gx_section_fast_forward": ("Fast Forward", "Vorspulen", "Avance rápido",
                                             "Avance rapide", "早送り", "Avanço rápido",
                                             "Перемотка", "快进"),
    "settings_genesis_plus_gx_fast_forward_speed": ("Fast forward speed", "Vorspul-Geschwindigkeit",
                                           "Velocidad de avance rápido", "Vitesse d'avance rapide",
                                           "早送りの速度", "Velocidade do avanço rápido",
                                           "Скорость перемотки", "快进速度"),
    "settings_genesis_plus_gx_fast_forward_mode": ("Fast forward mode", "Vorspul-Modus",
                                          "Modo de avance rápido", "Mode d'avance rapide",
                                          "早送りモード", "Modo do avanço rápido",
                                          "Режим перемотки", "快进模式"),
    "settings_genesis_plus_gx_fast_forward_hotkey": ("Fast forward button", "Vorspul-Taste",
                                            "Botón de avance rápido", "Bouton d'avance rapide",
                                            "早送りボタン", "Botão do avanço rápido",
                                            "Кнопка перемотки", "快进按键"),
    "settings_genesis_plus_gx_tab_controls": ("Controls", "Steuerung", "Controles", "Commandes", "操作",
                                     "Controles", "Управление", "控制"),
    "settings_genesis_plus_gx_section_button_mapping": ("Button mapping", "Tastenbelegung",
                                               "Asignación de botones", "Attribution des boutons",
                                               "ボタン割り当て", "Mapeamento de botões",
                                               "Назначение кнопок", "按键映射"),
    "settings_genesis_plus_gx_map_a": ("A", "A", "A", "A", "A", "A", "A", "A"),
    "settings_genesis_plus_gx_map_b": ("B", "B", "B", "B", "B", "B", "B", "B"),
    "settings_genesis_plus_gx_map_c": ("C", "C", "C", "C", "C", "C", "C", "C"),
    "settings_genesis_plus_gx_map_x": ("X", "X", "X", "X", "X", "X", "X", "X"),
    "settings_genesis_plus_gx_map_y": ("Y", "Y", "Y", "Y", "Y", "Y", "Y", "Y"),
    "settings_genesis_plus_gx_map_z": ("Z", "Z", "Z", "Z", "Z", "Z", "Z", "Z"),
    "settings_genesis_plus_gx_map_start": ("Start", "Start", "Start", "Start", "スタート", "Start",
                                  "Start", "开始"),
    "settings_genesis_plus_gx_map_mode": ("Mode", "Mode", "Mode", "Mode", "モード", "Mode",
                                         "Mode", "模式"),
    "settings_genesis_plus_gx_map_up": ("D-Pad Up", "Steuerkreuz oben", "Cruceta arriba",
                               "Croix haut", "十字キー上", "Direcional para cima",
                               "Крестовина вверх", "方向键上"),
    "settings_genesis_plus_gx_map_down": ("D-Pad Down", "Steuerkreuz unten", "Cruceta abajo",
                                 "Croix bas", "十字キー下", "Direcional para baixo",
                                 "Крестовина вниз", "方向键下"),
    "settings_genesis_plus_gx_map_left": ("D-Pad Left", "Steuerkreuz links", "Cruceta izquierda",
                                 "Croix gauche", "十字キー左", "Direcional para a esquerda",
                                 "Крестовина влево", "方向键左"),
    "settings_genesis_plus_gx_map_right": ("D-Pad Right", "Steuerkreuz rechts", "Cruceta derecha",
                                  "Croix droite", "十字キー右", "Direcional para a direita",
                                  "Крестовина вправо", "方向键右"),
    "settings_genesis_plus_gx_analog_dpad": ("Left stick as D-Pad", "Linker Stick als Steuerkreuz",
                                    "Stick izquierdo como cruceta",
                                    "Stick gauche comme croix directionnelle",
                                    "左スティックを十字キーとして使う",
                                    "Analógico esquerdo como direcional",
                                    "Левый стик как крестовина", "左摇杆作为方向键"),
}

# Choice labels the core does not translate. English -> (de, es, fr, ja, pt, ru, zh)
CHOICES = {
    "Disabled": ("Deaktiviert", "Desactivado", "Désactivé", "無効", "Desativado", "Выключено",
                 "禁用"),
    "Integer": ("Ganzzahlig", "Entero", "Entier", "整数倍", "Inteiro", "Целочисленный", "整数"),
    "Display": ("Anzeige", "Pantalla", "Écran", "画面", "Tela", "Экран", "屏幕"),
    "Stretch": ("Strecken", "Estirar", "Étirer", "引き伸ばし", "Esticar", "Растянуть", "拉伸"),
    "Original": ("Original", "Original", "Original", "オリジナル", "Original", "Оригинал", "原始"),
    "Auto": ("Auto", "Auto", "Auto", "自動", "Auto", "Авто", "自动"),
    "Unlimited": ("Unbegrenzt", "Ilimitado", "Illimité", "無制限", "Ilimitado", "Без ограничений",
                  "无限制"),
    "Hold": ("Halten", "Mantener", "Maintenir", "長押し", "Segurar", "Удерживать", "按住"),
    "Toggle": ("Umschalten", "Alternar", "Basculer", "切り替え", "Alternar", "Переключать", "切换"),
    "Right stick": ("Rechter Stick", "Stick derecho", "Stick droit", "右スティック", "Analógico direito",
                    "Правый стик", "右摇杆"),
    "Left stick": ("Linker Stick", "Stick izquierdo", "Stick gauche", "左スティック", "Analógico esquerdo",
                   "Левый стик", "左摇杆"),
    "Plus": ("Plus", "Más", "Plus", "プラス", "Mais", "Плюс", "加号"),
    "Minus": ("Minus", "Menos", "Moins", "マイナス", "Menos", "Минус", "减号"),
    "Up": ("Oben", "Arriba", "Haut", "上", "Cima", "Вверх", "上"),
    "Down": ("Unten", "Abajo", "Bas", "下", "Baixo", "Вниз", "下"),
    "Left": ("Links", "Izquierda", "Gauche", "左", "Esquerda", "Влево", "左"),
    "Right": ("Rechts", "Derecha", "Droite", "右", "Direita", "Вправо", "右"),
    "Hidden": ("Ausgeblendet", "Oculto", "Masqué", "非表示", "Oculto", "Скрыто", "隐藏"),
    "Top left": ("Oben links", "Arriba a la izquierda", "En haut à gauche", "左上",
                 "Superior esquerdo", "Сверху слева", "左上"),
    "Top right": ("Oben rechts", "Arriba a la derecha", "En haut à droite", "右上",
                  "Superior direito", "Сверху справа", "右上"),
    "Bottom left": ("Unten links", "Abajo a la izquierda", "En bas à gauche", "左下",
                    "Inferior esquerdo", "Снизу слева", "左下"),
    "Bottom right": ("Unten rechts", "Abajo a la derecha", "En bas à droite", "右下",
                     "Inferior direito", "Снизу справа", "右下"),
}

RESTART_SUFFIX = re.compile(r"\s*\((Restart Required|Reload Core|[^)]*[Nn]eustart[^)]*|[^)]*[Rr]einici[^)]*|"
                            r"[^)]*[Rr]edémarr[^)]*|[^)]*再起動[^)]*|[^)]*перезапуск[^)]*|"
                            r"[^)]*重启[^)]*|[^)]*[Rr]einicializa[^)]*)\)")


def value_key(label: str) -> str:
    """tico_config.cpp's ValueKey: settings_genesis_plus_gx_value_ + label as a slug."""
    return "settings_genesis_plus_gx_value_" + "_".join(re.findall(r"[a-z0-9]+", label.lower()))


def clean_label(text: str) -> str:
    return RESTART_SUFFIX.sub("", text).lstrip("> ").strip()


def english_choice(value: str, label: str) -> str:
    # the core leaves on/off style values unlabelled
    return label.capitalize() if label == value and value in ("disabled", "enabled") else label


def dump_options() -> dict:
    with tempfile.TemporaryDirectory() as tmp:
        exe = Path(tmp) / "dump_core_options"
        # the defines Makefile.libretro builds the core with, which add
        # options (overclock, EQ, Nuked cores, per-channel volumes)
        subprocess.run(["cc", "-std=gnu11", "-w", "-DM68K_OVERCLOCK_SHIFT=20",
                        "-DZ80_OVERCLOCK_SHIFT=20", "-DHAVE_YM3438_CORE", "-DHAVE_OPLL_CORE",
                        "-DUSE_PER_SOUND_CHANNELS_CONFIG",
                        "-I", str(ROOT / "libretro"),
                        "-I", str(ROOT / "libretro/libretro-common/include"),
                        "-o", str(exe), str(Path(__file__).with_name("dump_core_options.c"))],
                       check=True)
        return json.loads(subprocess.run([str(exe)], check=True, capture_output=True,
                                         text=True).stdout)


def label_key(key: str) -> str:
    return LABEL_KEYS.get(key, "settings_genesis_plus_gx_" + key.removeprefix("genesis_plus_gx_"))


def build_settings(dump: dict) -> dict:
    core = {o["key"]: o for o in dump["en"]}
    placed = {key for _, sections in LAYOUT for _, keys in sections for key in keys}
    missing = sorted(set(core) - placed - EXCLUDED)
    if missing:
        raise SystemExit(f"core options not placed in LAYOUT: {missing}")

    tabs = []
    for tab, sections in LAYOUT:
        out_sections = []
        for title, keys in sections:
            options = []
            for key in keys:
                source = core[key]
                values = [v for v, _ in source["values"]]
                default = source["default"]
                if default not in values:
                    # the core's default names a label (CPU Speed's "100%")
                    default = next(v for v, l in source["values"] if l == default)
                option = {"key": key, "label": label_key(key)}
                if sorted(values) == ["disabled", "enabled"]:
                    option.update(type="bool", default=default)
                else:
                    option.update(type="enum", default=default, choices=[
                        {"label": english_choice(v, l), "value": v} for v, l in source["values"]])
                if RESTART_SUFFIX.search(source["desc"]):
                    option["restart"] = True
                if key in DEPENDS_ON:
                    on, value = DEPENDS_ON[key]
                    option["depends_on"] = {"key": on, "value": value}
                options.append(option)
            out_sections.append({"title": title, "options": options})
        tabs.append({"name": tab, "sections": out_sections})

    def overlay_tab(definition):
        tab, sections = definition
        return {"name": tab, "sections": [
            {"title": title, "options": [
                {**o, "choices": [{"label": l, "value": v} for v, l in o["choices"]]}
                if "choices" in o else dict(o)
                for o in options]}
            for title, options in sections]}

    tabs.insert(1, overlay_tab(OVERLAY_TAB))
    tabs.append(overlay_tab(CONTROLS_TAB))

    return {
        "core_id": "genesis_plus_gx",
        "display_name": "Genesis Plus GX",
        "config_file": "genesis_plus_gx.jsonc",
        "slugs": ["genesis", "master-system", "game-gear", "sega-cd"],
        "bool_true_value": "enabled",
        "bool_false_value": "disabled",
        "tabs": tabs,
    }


def tico_owned(key: str) -> bool:
    """Labels tico itself defines, whose wording tico keeps."""
    return key in LABEL_KEYS.values() or key.startswith(("settings_genesis_plus_gx_tab_",
                                                          "settings_genesis_plus_gx_section_"))


def build_strings(dump: dict, settings: dict,
                  existing: dict[str, dict[str, str]]) -> dict[str, dict[str, str]]:
    strings: dict[str, dict[str, str]] = {lang: {} for lang in LANGUAGES}
    english = {o["key"]: o for o in dump["en"]}
    for lang in LANGUAGES:
        out = strings[lang]
        index = LANGUAGES.index(lang)
        for option in dump[lang]:
            if option["key"] in EXCLUDED:
                continue
            key, en = option["key"], english[option["key"]]
            out[label_key(key)] = clean_label(option["desc"])
            for (value, label), (_, en_label) in zip(option["values"], en["values"]):
                en_text = english_choice(value, en_label)
                if label != en_label:
                    out[value_key(en_text)] = label
        for key, texts in LABELS.items():
            out[key] = texts[index]
        if lang != "en":
            for choice, translations in CHOICES.items():
                out[value_key(choice)] = translations[index - 1]
        if lang == "en":
            for key in list(out):
                if key.startswith("settings_genesis_plus_gx_value_"):
                    del out[key]
    # tico's own labels keep tico's wording: from tico-nx when it is checked
    # out, otherwise as the language files already have them
    for lang in LANGUAGES:
        path = TICO_NX / "assets/lang" / f"{lang}.json"
        source = json.loads(path.read_text()) if path.exists() else existing[lang]
        for key, value in source.items():
            if tico_owned(key):
                strings[lang][key] = value
    used = set()

    def walk(node):
        if isinstance(node, dict):
            for field in ("label", "name", "title"):
                if isinstance(node.get(field), str) and node[field].startswith("settings_"):
                    used.add(node[field])
            for child in node.values():
                walk(child)
        elif isinstance(node, list):
            for child in node:
                walk(child)

    walk(settings)
    unlabelled = sorted(used - set(strings["en"]))
    if unlabelled:
        raise SystemExit(f"labels without English text: {unlabelled}")
    return strings


def main() -> None:
    dump = dump_options()
    settings = build_settings(dump)
    SETTINGS.write_text(json.dumps(settings, indent=2, ensure_ascii=False) + "\n")
    current_files = {lang: json.loads((LANG_DIR / f"{lang}.json").read_text())
                     if (LANG_DIR / f"{lang}.json").exists() else {} for lang in LANGUAGES}
    for lang, strings in build_strings(dump, settings, current_files).items():
        path = LANG_DIR / f"{lang}.json"
        current = {k: v for k, v in current_files[lang].items()
                   if not k.startswith("settings_genesis_plus_gx_")}
        current.update(dict(sorted(strings.items())))
        path.write_text(json.dumps(current, indent=4, ensure_ascii=False) + "\n")
    print(f"wrote {SETTINGS.relative_to(ROOT)} and {len(LANGUAGES)} language files")


if __name__ == "__main__":
    main()
