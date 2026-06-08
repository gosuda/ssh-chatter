
typedef struct {
    const char *name;
    const char *code;
} color_entry_t;

static const color_entry_t USER_COLOR_MAP[] = {
    {"black", ANSI_BLACK},
    {"red", ANSI_RED},
    {"green", ANSI_GREEN},
    {"yellow", ANSI_YELLOW},
    {"blue", ANSI_BLUE},
    {"magenta", ANSI_MAGENTA},
    {"cyan", ANSI_CYAN},
    {"white", ANSI_WHITE},
    {"default", ANSI_DEFAULT},

    {"검정", ANSI_BLACK},
    {"검은색", ANSI_BLACK},
    {"黒", ANSI_BLACK},
    {"黑", ANSI_BLACK},
    {"黑色", ANSI_BLACK},
    {"черный", ANSI_BLACK},
    {"чёрный", ANSI_BLACK},

    {"빨강", ANSI_RED},
    {"빨간색", ANSI_RED},
    {"赤", ANSI_RED},
    {"红", ANSI_RED},
    {"红色", ANSI_RED},
    {"красный", ANSI_RED},

    {"초록", ANSI_GREEN},
    {"초록색", ANSI_GREEN},
    {"緑", ANSI_GREEN},
    {"绿", ANSI_GREEN},
    {"绿色", ANSI_GREEN},
    {"зелёный", ANSI_GREEN},
    {"зеленый", ANSI_GREEN},

    {"노랑", ANSI_YELLOW},
    {"노란색", ANSI_YELLOW},
    {"黄色", ANSI_YELLOW},
    {"黄", ANSI_YELLOW},
    {"黄色い", ANSI_YELLOW},
    {"黃色", ANSI_YELLOW},
    {"жёлтый", ANSI_YELLOW},
    {"желтый", ANSI_YELLOW},

    {"파랑", ANSI_BLUE},
    {"파란색", ANSI_BLUE},
    {"青", ANSI_BLUE},
    {"青色", ANSI_BLUE},
    {"蓝", ANSI_BLUE},
    {"蓝色", ANSI_BLUE},
    {"синий", ANSI_BLUE},

    {"마젠타", ANSI_MAGENTA},
    {"자주", ANSI_MAGENTA},
    {"보라", ANSI_MAGENTA},
    {"보라색", ANSI_MAGENTA},
    {"マゼンタ", ANSI_MAGENTA},
    {"紫", ANSI_MAGENTA},
    {"洋红", ANSI_MAGENTA},
    {"品红", ANSI_MAGENTA},
    {"紫色", ANSI_MAGENTA},
    {"пурпурный", ANSI_MAGENTA},
    {"фиолетовый", ANSI_MAGENTA},

    {"시안", ANSI_CYAN},
    {"청록", ANSI_CYAN},
    {"하늘", ANSI_CYAN},
    {"하늘색", ANSI_CYAN},
    {"シアン", ANSI_CYAN},
    {"水色", ANSI_CYAN},
    {"青绿", ANSI_CYAN},
    {"青色", ANSI_CYAN},
    {"青綠", ANSI_CYAN},
    {"青藍", ANSI_CYAN},
    {"青蓝", ANSI_CYAN},
    {"циан", ANSI_CYAN},
    {"бирюзовый", ANSI_CYAN},
    {"голубой", ANSI_CYAN},

    {"하양", ANSI_WHITE},
    {"흰색", ANSI_WHITE},
    {"白", ANSI_WHITE},
    {"白色", ANSI_WHITE},
    {"белый", ANSI_WHITE},

    {"기본", ANSI_DEFAULT},
    {"기본값", ANSI_DEFAULT},
    {"デフォルト", ANSI_DEFAULT},
    {"既定", ANSI_DEFAULT},
    {"默认", ANSI_DEFAULT},
    {"默認", ANSI_DEFAULT},
    {"по умолчанию", ANSI_DEFAULT},

    {"bright-black", ANSI_BRIGHT_BLACK},
    {"bright-red", ANSI_BRIGHT_RED},
    {"bright-green", ANSI_BRIGHT_GREEN},
    {"bright-yellow", ANSI_BRIGHT_YELLOW},
    {"bright-blue", ANSI_BRIGHT_BLUE},
    {"bright-magenta", ANSI_BRIGHT_MAGENTA},
    {"bright-cyan", ANSI_BRIGHT_CYAN},
    {"bright-white", ANSI_BRIGHT_WHITE},

    {"밝은검정", ANSI_BRIGHT_BLACK},
    {"회색", ANSI_BRIGHT_BLACK},
    {"明るい黒", ANSI_BRIGHT_BLACK},
    {"グレー", ANSI_BRIGHT_BLACK},
    {"灰色", ANSI_BRIGHT_BLACK},
    {"серый", ANSI_BRIGHT_BLACK},

    {"밝은빨강", ANSI_BRIGHT_RED},
    {"밝은빨간색", ANSI_BRIGHT_RED},
    {"明るい赤", ANSI_BRIGHT_RED},
    {"亮红", ANSI_BRIGHT_RED},
    {"亮红色", ANSI_BRIGHT_RED},
    {"ярко-красный", ANSI_BRIGHT_RED},

    {"밝은초록", ANSI_BRIGHT_GREEN},
    {"밝은초록색", ANSI_BRIGHT_GREEN},
    {"明るい緑", ANSI_BRIGHT_GREEN},
    {"亮绿", ANSI_BRIGHT_GREEN},
    {"亮绿色", ANSI_BRIGHT_GREEN},
    {"ярко-зелёный", ANSI_BRIGHT_GREEN},
    {"ярко-зеленый", ANSI_BRIGHT_GREEN},

    {"밝은노랑", ANSI_BRIGHT_YELLOW},
    {"밝은노란색", ANSI_BRIGHT_YELLOW},
    {"明るい黄", ANSI_BRIGHT_YELLOW},
    {"亮黄", ANSI_BRIGHT_YELLOW},
    {"亮黄色", ANSI_BRIGHT_YELLOW},
    {"ярко-жёлтый", ANSI_BRIGHT_YELLOW},
    {"ярко-желтый", ANSI_BRIGHT_YELLOW},

    {"밝은파랑", ANSI_BRIGHT_BLUE},
    {"밝은파란색", ANSI_BRIGHT_BLUE},
    {"明るい青", ANSI_BRIGHT_BLUE},
    {"亮蓝", ANSI_BRIGHT_BLUE},
    {"亮蓝色", ANSI_BRIGHT_BLUE},
    {"ярко-синий", ANSI_BRIGHT_BLUE},

    {"밝은마젠타", ANSI_BRIGHT_MAGENTA},
    {"밝은자주", ANSI_BRIGHT_MAGENTA},
    {"밝은보라", ANSI_BRIGHT_MAGENTA},
    {"밝은보라색", ANSI_BRIGHT_MAGENTA},
    {"明るいマゼンタ", ANSI_BRIGHT_MAGENTA},
    {"明るい紫", ANSI_BRIGHT_MAGENTA},
    {"亮洋红", ANSI_BRIGHT_MAGENTA},
    {"亮品红", ANSI_BRIGHT_MAGENTA},
    {"亮紫色", ANSI_BRIGHT_MAGENTA},
    {"ярко-пурпурный", ANSI_BRIGHT_MAGENTA},
    {"ярко-фиолетовый", ANSI_BRIGHT_MAGENTA},

    {"밝은시안", ANSI_BRIGHT_CYAN},
    {"밝은청록", ANSI_BRIGHT_CYAN},
    {"밝은하늘", ANSI_BRIGHT_CYAN},
    {"밝은하늘색", ANSI_BRIGHT_CYAN},
    {"明るいシアン", ANSI_BRIGHT_CYAN},
    {"明るい水色", ANSI_BRIGHT_CYAN},
    {"亮青色", ANSI_BRIGHT_CYAN},
    {"亮青绿", ANSI_BRIGHT_CYAN},
    {"亮青綠", ANSI_BRIGHT_CYAN},
    {"亮青藍", ANSI_BRIGHT_CYAN},
    {"亮青蓝", ANSI_BRIGHT_CYAN},
    {"ярко-циан", ANSI_BRIGHT_CYAN},
    {"ярко-бирюзовый", ANSI_BRIGHT_CYAN},
    {"ярко-голубой", ANSI_BRIGHT_CYAN},

    {"밝은하양", ANSI_BRIGHT_WHITE},
    {"밝은흰색", ANSI_BRIGHT_WHITE},
    {"明るい白", ANSI_BRIGHT_WHITE},
    {"亮白", ANSI_BRIGHT_WHITE},
    {"亮白色", ANSI_BRIGHT_WHITE},
    {"ярко-белый", ANSI_BRIGHT_WHITE},
};

static const color_entry_t HIGHLIGHT_COLOR_MAP[] = {
    {"black", ANSI_BG_BLACK},
    {"red", ANSI_BG_RED},
    {"green", ANSI_BG_GREEN},
    {"yellow", ANSI_BG_YELLOW},
    {"blue", ANSI_BG_BLUE},
    {"magenta", ANSI_BG_MAGENTA},
    {"cyan", ANSI_BG_CYAN},
    {"white", ANSI_BG_WHITE},
    {"default", ANSI_BG_DEFAULT},

    {"검정", ANSI_BG_BLACK},
    {"검은색", ANSI_BG_BLACK},
    {"黒", ANSI_BG_BLACK},
    {"黑", ANSI_BG_BLACK},
    {"黑色", ANSI_BG_BLACK},
    {"черный", ANSI_BG_BLACK},
    {"чёрный", ANSI_BG_BLACK},

    {"빨강", ANSI_BG_RED},
    {"빨간색", ANSI_BG_RED},
    {"赤", ANSI_BG_RED},
    {"红", ANSI_BG_RED},
    {"红色", ANSI_BG_RED},
    {"красный", ANSI_BG_RED},

    {"초록", ANSI_BG_GREEN},
    {"초록색", ANSI_BG_GREEN},
    {"緑", ANSI_BG_GREEN},
    {"绿", ANSI_BG_GREEN},
    {"绿色", ANSI_BG_GREEN},
    {"зелёный", ANSI_BG_GREEN},
    {"зеленый", ANSI_BG_GREEN},

    {"노랑", ANSI_BG_YELLOW},
    {"노란색", ANSI_BG_YELLOW},
    {"黄色", ANSI_BG_YELLOW},
    {"黄", ANSI_BG_YELLOW},
    {"黄色い", ANSI_BG_YELLOW},
    {"黃色", ANSI_BG_YELLOW},
    {"жёлтый", ANSI_BG_YELLOW},
    {"желтый", ANSI_BG_YELLOW},

    {"파랑", ANSI_BG_BLUE},
    {"파란색", ANSI_BG_BLUE},
    {"青", ANSI_BG_BLUE},
    {"青色", ANSI_BG_BLUE},
    {"蓝", ANSI_BG_BLUE},
    {"蓝色", ANSI_BG_BLUE},
    {"синий", ANSI_BG_BLUE},

    {"마젠타", ANSI_BG_MAGENTA},
    {"자주", ANSI_BG_MAGENTA},
    {"보라", ANSI_BG_MAGENTA},
    {"보라색", ANSI_BG_MAGENTA},
    {"マゼンタ", ANSI_BG_MAGENTA},
    {"紫", ANSI_BG_MAGENTA},
    {"洋红", ANSI_BG_MAGENTA},
    {"品红", ANSI_BG_MAGENTA},
    {"紫色", ANSI_BG_MAGENTA},
    {"пурпурный", ANSI_BG_MAGENTA},
    {"фиолетовый", ANSI_BG_MAGENTA},

    {"시안", ANSI_BG_CYAN},
    {"청록", ANSI_BG_CYAN},
    {"하늘", ANSI_BG_CYAN},
    {"하늘색", ANSI_BG_CYAN},
    {"シアン", ANSI_BG_CYAN},
    {"水色", ANSI_BG_CYAN},
    {"青绿", ANSI_BG_CYAN},
    {"青色", ANSI_BG_CYAN},
    {"青綠", ANSI_BG_CYAN},
    {"青藍", ANSI_BG_CYAN},
    {"青蓝", ANSI_BG_CYAN},
    {"циан", ANSI_BG_CYAN},
    {"бирюзовый", ANSI_BG_CYAN},
    {"голубой", ANSI_BG_CYAN},

    {"하양", ANSI_BG_WHITE},
    {"흰색", ANSI_BG_WHITE},
    {"白", ANSI_BG_WHITE},
    {"白色", ANSI_BG_WHITE},
    {"белый", ANSI_BG_WHITE},

    {"기본", ANSI_BG_DEFAULT},
    {"기본값", ANSI_BG_DEFAULT},
    {"デフォルト", ANSI_BG_DEFAULT},
    {"既定", ANSI_BG_DEFAULT},
    {"默认", ANSI_BG_DEFAULT},
    {"默認", ANSI_BG_DEFAULT},
    {"по умолчанию", ANSI_BG_DEFAULT},

    {"bright-black", ANSI_BG_BRIGHT_BLACK},
    {"bright-red", ANSI_BG_BRIGHT_RED},
    {"bright-green", ANSI_BG_BRIGHT_GREEN},
    {"bright-yellow", ANSI_BG_BRIGHT_YELLOW},
    {"bright-blue", ANSI_BG_BRIGHT_BLUE},
    {"bright-magenta", ANSI_BG_BRIGHT_MAGENTA},
    {"bright-cyan", ANSI_BG_BRIGHT_CYAN},
    {"bright-white", ANSI_BG_BRIGHT_WHITE},

    {"밝은검정", ANSI_BG_BRIGHT_BLACK},
    {"회색", ANSI_BG_BRIGHT_BLACK},
    {"明るい黒", ANSI_BG_BRIGHT_BLACK},
    {"グレー", ANSI_BG_BRIGHT_BLACK},
    {"灰色", ANSI_BG_BRIGHT_BLACK},
    {"серый", ANSI_BG_BRIGHT_BLACK},

    {"밝은빨강", ANSI_BG_BRIGHT_RED},
    {"밝은빨간색", ANSI_BG_BRIGHT_RED},
    {"明るい赤", ANSI_BG_BRIGHT_RED},
    {"亮红", ANSI_BG_BRIGHT_RED},
    {"亮红色", ANSI_BG_BRIGHT_RED},
    {"ярко-красный", ANSI_BG_BRIGHT_RED},

    {"밝은초록", ANSI_BG_BRIGHT_GREEN},
    {"밝은초록색", ANSI_BG_BRIGHT_GREEN},
    {"明るい緑", ANSI_BG_BRIGHT_GREEN},
    {"亮绿", ANSI_BG_BRIGHT_GREEN},
    {"亮绿色", ANSI_BG_BRIGHT_GREEN},
    {"ярко-зелёный", ANSI_BG_BRIGHT_GREEN},
    {"ярко-зеленый", ANSI_BG_BRIGHT_GREEN},

    {"밝은노랑", ANSI_BG_BRIGHT_YELLOW},
    {"밝은노란색", ANSI_BG_BRIGHT_YELLOW},
    {"明るい黄", ANSI_BG_BRIGHT_YELLOW},
    {"亮黄", ANSI_BG_BRIGHT_YELLOW},
    {"亮黄色", ANSI_BG_BRIGHT_YELLOW},
    {"ярко-жёлтый", ANSI_BG_BRIGHT_YELLOW},
    {"ярко-желтый", ANSI_BG_BRIGHT_YELLOW},

    {"밝은파랑", ANSI_BG_BRIGHT_BLUE},
    {"밝은파란색", ANSI_BG_BRIGHT_BLUE},
    {"明るい青", ANSI_BG_BRIGHT_BLUE},
    {"亮蓝", ANSI_BG_BRIGHT_BLUE},
    {"亮蓝色", ANSI_BG_BRIGHT_BLUE},
    {"ярко-синий", ANSI_BG_BRIGHT_BLUE},

    {"밝은마젠타", ANSI_BG_BRIGHT_MAGENTA},
    {"밝은자주", ANSI_BG_BRIGHT_MAGENTA},
    {"밝은보라", ANSI_BG_BRIGHT_MAGENTA},
    {"밝은보라색", ANSI_BG_BRIGHT_MAGENTA},
    {"明るいマゼンタ", ANSI_BG_BRIGHT_MAGENTA},
    {"明るい紫", ANSI_BG_BRIGHT_MAGENTA},
    {"亮洋红", ANSI_BG_BRIGHT_MAGENTA},
    {"亮品红", ANSI_BG_BRIGHT_MAGENTA},
    {"亮紫色", ANSI_BG_BRIGHT_MAGENTA},
    {"ярко-пурпурный", ANSI_BG_BRIGHT_MAGENTA},
    {"ярко-фиолетовый", ANSI_BG_BRIGHT_MAGENTA},

    {"밝은시안", ANSI_BG_BRIGHT_CYAN},
    {"밝은청록", ANSI_BG_BRIGHT_CYAN},
    {"밝은하늘", ANSI_BG_BRIGHT_CYAN},
    {"밝은하늘색", ANSI_BG_BRIGHT_CYAN},
    {"明るいシアン", ANSI_BG_BRIGHT_CYAN},
    {"明るい水色", ANSI_BG_BRIGHT_CYAN},
    {"亮青色", ANSI_BG_BRIGHT_CYAN},
    {"亮青绿", ANSI_BG_BRIGHT_CYAN},
    {"亮青綠", ANSI_BG_BRIGHT_CYAN},
    {"亮青藍", ANSI_BG_BRIGHT_CYAN},
    {"亮青蓝", ANSI_BG_BRIGHT_CYAN},
    {"ярко-циан", ANSI_BG_BRIGHT_CYAN},
    {"ярко-бирюзовый", ANSI_BG_BRIGHT_CYAN},
    {"ярко-голубой", ANSI_BG_BRIGHT_CYAN},

    {"밝은하양", ANSI_BG_BRIGHT_WHITE},
    {"밝은흰색", ANSI_BG_BRIGHT_WHITE},
    {"明るい白", ANSI_BG_BRIGHT_WHITE},
    {"亮白", ANSI_BG_BRIGHT_WHITE},
    {"亮白色", ANSI_BG_BRIGHT_WHITE},
    {"ярко-белый", ANSI_BG_BRIGHT_WHITE},
};

#include "ssh_chatter/l10n.h"

typedef struct palette_descriptor {
    const char *id;
    const l10n_string_t *name;
    const l10n_string_t *description;
    const char *user_color_name;
    const char *user_highlight_name;
    bool user_is_bold;
    const char *system_fg_name;
    const char *system_bg_name;
    const char *system_highlight_name;
    bool system_is_bold;
    bool is_256_color;
} palette_descriptor_t;

#define L10N_ALL(str)                                                          \
    {                                                                          \
        (str), (str), (str), (str), (str), (str)                               \
    }

static const l10n_string_t PALETTE_NAME_MONOKAI = L10N_ALL("monokai");
static const l10n_string_t PALETTE_DESC_MONOKAI =
    L10N_ALL("A dark theme with vibrant accent colors, inspired by the "
             "Monokai color scheme.");

static const l10n_string_t PALETTE_NAME_WINDOWS = L10N_ALL("windows");
static const l10n_string_t PALETTE_DESC_WINDOWS =
    L10N_ALL("High contrast palette reminiscent of Windows");
static const l10n_string_t PALETTE_NAME_GNU_LINUX = L10N_ALL("gnu-linux");
static const l10n_string_t PALETTE_DESC_GNU_LINUX = L10N_ALL(
    "Modern, elegant, and free. the universal rhythm of your workflow.");
static const l10n_string_t PALETTE_NAME_MACOS = L10N_ALL("macos");
static const l10n_string_t PALETTE_DESC_MACOS =
    L10N_ALL("Precision in silence. Minimalist contemporary unix.");
static const l10n_string_t PALETTE_NAME_FREEBSD = L10N_ALL("freebsd");
static const l10n_string_t PALETTE_DESC_FREEBSD =
    L10N_ALL("Rigid and graceful BSD. The biggest 'True UNIX'");
static const l10n_string_t PALETTE_NAME_SOLARIS = L10N_ALL("solaris");
static const l10n_string_t PALETTE_DESC_SOLARIS =
    L10N_ALL("Ancient sun of enterprise UNIX: Sun, Machine, nostalgia.");
static const l10n_string_t PALETTE_NAME_OPENBSD_FORTRESS =
    L10N_ALL("openbsd-fortress");
static const l10n_string_t PALETTE_DESC_OPENBSD_FORTRESS = L10N_ALL(
    "Security through simplicity. calm blue walls over disciplined darkness.");
static const l10n_string_t PALETTE_NAME_NETBSD_UNIVERSAL =
    L10N_ALL("netbsd-universal");
static const l10n_string_t PALETTE_DESC_NETBSD_UNIVERSAL =
    L10N_ALL("Runs on anything. Maybe your fridge, too?");
static const l10n_string_t PALETTE_NAME_MOE = L10N_ALL("moe");
static const l10n_string_t PALETTE_DESC_MOE =
    L10N_ALL("Soft magenta accents with playful highlights");
static const l10n_string_t PALETTE_NAME_NEON_GENESIS_EVANGELION =
    L10N_ALL("neon-genesis-evangelion");
static const l10n_string_t PALETTE_DESC_NEON_GENESIS_EVANGELION =
    L10N_ALL("Sho-nen yo Shin-wa ni nare--");
static const l10n_string_t PALETTE_NAME_MEGAMI = L10N_ALL("megami");
static const l10n_string_t PALETTE_DESC_MEGAMI =
    L10N_ALL("Japanese anime goddess cliche");
static const l10n_string_t PALETTE_NAME_CLEAN = L10N_ALL("clean");
static const l10n_string_t PALETTE_DESC_CLEAN =
    L10N_ALL("Balanced neutral palette");
static const l10n_string_t PALETTE_NAME_ADWAITA = L10N_ALL("adwaita");
static const l10n_string_t PALETTE_DESC_ADWAITA =
    L10N_ALL("Bright background inspired by GNOME Adwaita");
static const l10n_string_t PALETTE_NAME_80SHACKER = L10N_ALL("80shacker");
static const l10n_string_t PALETTE_DESC_80SHACKER =
    L10N_ALL("Bright monochrome green inspired by old CRT");
static const l10n_string_t PALETTE_NAME_PLATO = L10N_ALL("plato");
static const l10n_string_t PALETTE_DESC_PLATO =
    L10N_ALL("Bright monochrome yellow inspired by old Amber CRT");
static const l10n_string_t PALETTE_NAME_ATARIST = L10N_ALL("atarist");
static const l10n_string_t PALETTE_DESC_ATARIST =
    L10N_ALL("Sharp paper-white monochrome for high-res work");
static const l10n_string_t PALETTE_NAME_WIN95BSOD = L10N_ALL("win95bsod");
static const l10n_string_t PALETTE_DESC_WIN95BSOD =
    L10N_ALL("High-contrast blue screen of death style");
static const l10n_string_t PALETTE_NAME_CHN_HANZI = L10N_ALL("chn-hanzi");
static const l10n_string_t PALETTE_DESC_CHN_HANZI =
    L10N_ALL("Bright cyan high-clarity Chinese text terminal");
static const l10n_string_t PALETTE_NAME_USA_FLAG = L10N_ALL("usa-flag");
static const l10n_string_t PALETTE_DESC_USA_FLAG =
    L10N_ALL("Flag blue base with red/white highlights");
static const l10n_string_t PALETTE_NAME_JPN_FLAG = L10N_ALL("jpn-flag");
static const l10n_string_t PALETTE_DESC_JPN_FLAG =
    L10N_ALL("Minimalist white with rising sun red accent");
static const l10n_string_t PALETTE_NAME_CHN_FLAG = L10N_ALL("chn-flag");
static const l10n_string_t PALETTE_DESC_CHN_FLAG =
    L10N_ALL("Star-red background with lucky yellow text");
static const l10n_string_t PALETTE_NAME_RUS_FLAG = L10N_ALL("rus-flag");
static const l10n_string_t PALETTE_DESC_RUS_FLAG =
    L10N_ALL("Tricolor base with strong red emphasis");
static const l10n_string_t PALETTE_NAME_DE_FLAG = L10N_ALL("de-flag");
static const l10n_string_t PALETTE_DESC_DE_FLAG =
    L10N_ALL("Tricolor base with strong red/yellow emphasis");
static const l10n_string_t PALETTE_NAME_HOLY_LIGHT = L10N_ALL("holy-light");
static const l10n_string_t PALETTE_DESC_HOLY_LIGHT =
    L10N_ALL("Christian sacred light on pure white/blue base");
static const l10n_string_t PALETTE_NAME_ISLAM = L10N_ALL("islam");
static const l10n_string_t PALETTE_DESC_ISLAM =
    L10N_ALL("Iconic color of muslim, white/green base");
static const l10n_string_t PALETTE_NAME_DHARMA_OCHRE = L10N_ALL("dharma-ochre");
static const l10n_string_t PALETTE_DESC_DHARMA_OCHRE =
    L10N_ALL("Ochre robes of enlightenment and vitality");
static const l10n_string_t PALETTE_NAME_YIN_YANG = L10N_ALL("yin-yang");
static const l10n_string_t PALETTE_DESC_YIN_YANG =
    L10N_ALL("Balance of Black and White with Jade accent");
static const l10n_string_t PALETTE_NAME_SOVIET_COLD = L10N_ALL("soviet-cold");
static const l10n_string_t PALETTE_DESC_SOVIET_COLD =
    L10N_ALL("Cold blue/white terminal for scientific systems");
static const l10n_string_t PALETTE_NAME_HI_TEL = L10N_ALL("hi-tel");
static const l10n_string_t PALETTE_DESC_HI_TEL =
    L10N_ALL("1990s Korean BBS blue background and text style");
static const l10n_string_t PALETTE_NAME_BLACKPINK = L10N_ALL("blackpink");
static const l10n_string_t PALETTE_DESC_BLACKPINK = L10N_ALL(
    "K-Pop inspired palette with a black background and pink accents.");
static const l10n_string_t PALETTE_NAME_AMIGA_CLI = L10N_ALL("amiga-cli");
static const l10n_string_t PALETTE_DESC_AMIGA_CLI =
    L10N_ALL("AmigaOS style with cyan/blue");
static const l10n_string_t PALETTE_NAME_JPN_PC98 = L10N_ALL("jpn-pc98");
static const l10n_string_t PALETTE_DESC_JPN_PC98 =
    L10N_ALL("NEC PC-9801 subtle, earthy low-res tones");
static const l10n_string_t PALETTE_NAME_DEEP_BLUE = L10N_ALL("deep-blue");
static const l10n_string_t PALETTE_DESC_DEEP_BLUE =
    L10N_ALL("IBM Supercomputer monitoring interface style");
static const l10n_string_t PALETTE_NAME_KOREA = L10N_ALL("korea");
static const l10n_string_t PALETTE_DESC_KOREA =
    L10N_ALL("Taegeuk-gi inspired black base with red and blue accents");
static const l10n_string_t PALETTE_NAME_NEO_SEOUL = L10N_ALL("neo-seoul");
static const l10n_string_t PALETTE_DESC_NEO_SEOUL =
    L10N_ALL("Neon skyline of Gangnam and Hongdae: glowing magenta and cyan "
             "lights on dark asphalt");
static const l10n_string_t PALETTE_NAME_INCHEON_INDUSTRIAL =
    L10N_ALL("incheon-industrial");
static const l10n_string_t PALETTE_DESC_INCHEON_INDUSTRIAL =
    L10N_ALL("Metallic cranes and sodium streetlights of Incheon docks");
static const l10n_string_t PALETTE_NAME_GYEONGGI_MODERN =
    L10N_ALL("gyeonggi-modern");
static const l10n_string_t PALETTE_DESC_GYEONGGI_MODERN = L10N_ALL(
    "Suburban calm of modern Korea. asphalt gray and warm window light");
static const l10n_string_t PALETTE_NAME_KOREAN_PALACE =
    L10N_ALL("korean-palace");
static const l10n_string_t PALETTE_DESC_KOREAN_PALACE =
    L10N_ALL("Royal dancheong harmony: jade green, vermilion red, and gold "
             "over black lacquer");
static const l10n_string_t PALETTE_NAME_GYEONGSANGBUKDO =
    L10N_ALL("gyeongsangbukdo");
static const l10n_string_t PALETTE_DESC_GYEONGSANGBUKDO =
    L10N_ALL("Stoic mountains and agricultural spirit. stone, pine, and the "
             "quiet gold of temples");
static const l10n_string_t PALETTE_NAME_DAEGU_SUMMER = L10N_ALL("daegu-summer");
static const l10n_string_t PALETTE_DESC_DAEGU_SUMMER =
    L10N_ALL("The biggest, the hottest of north gyeongsang: Blazing "
             "red-orange heat and festival gold under night sky");
static const l10n_string_t PALETTE_NAME_GYEONGJU_HERITAGE =
    L10N_ALL("gyeongju-heritage");
static const l10n_string_t PALETTE_DESC_GYEONGJU_HERITAGE =
    L10N_ALL("Eternal relics and golden crowns: moonlit stone and ancient "
             "buddhism with blue flag of shilla military force");
static const l10n_string_t PALETTE_NAME_KANGWON_WINTER =
    L10N_ALL("kangwon-winter");
static const l10n_string_t PALETTE_DESC_KANGWON_WINTER =
    L10N_ALL("Cold white peaks and blue shadows of Gangwon's frozen dawn");
static const l10n_string_t PALETTE_NAME_ULSAN_STEEL = L10N_ALL("ulsan-steel");
static const l10n_string_t PALETTE_DESC_ULSAN_STEEL =
    L10N_ALL("Molten metal glow inside heavy industry furnace halls");
static const l10n_string_t PALETTE_NAME_JEOLLA_SEASIDE =
    L10N_ALL("jeolla-seaside");
static const l10n_string_t PALETTE_DESC_JEOLLA_SEASIDE =
    L10N_ALL("Quiet sea and horizon light of Mokpo and Yeosu nights");
static const l10n_string_t PALETTE_NAME_GWANGJU_BIENNALE =
    L10N_ALL("gwangju-biennale");
static const l10n_string_t PALETTE_DESC_GWANGJU_BIENNALE =
    L10N_ALL("Experimental art city with a heritage of democracy: violet "
             "neon and philosophical blue");
static const l10n_string_t PALETTE_NAME_JEONJU_HANOK = L10N_ALL("jeonju-hanok");
static const l10n_string_t PALETTE_DESC_JEONJU_HANOK =
    L10N_ALL("The symbol of north jeolla. warm roofs and calm golden light");
static const l10n_string_t PALETTE_NAME_DAEJEON_TECH = L10N_ALL("daejeon-tech");
static const l10n_string_t PALETTE_DESC_DAEJEON_TECH = L10N_ALL(
    "Futuristic research district glow: clean LED light on steel gray night");
static const l10n_string_t PALETTE_NAME_SEJONG_NIGHT = L10N_ALL("sejong-night");
static const l10n_string_t PALETTE_DESC_SEJONG_NIGHT =
    L10N_ALL("Balanced dark-blue administration city under cool LED light");
static const l10n_string_t PALETTE_NAME_CHEONGJU_INTELLECT =
    L10N_ALL("cheongju-intellect");
static const l10n_string_t PALETTE_DESC_CHEONGJU_INTELLECT =
    L10N_ALL("Scholarly ink and soft dawn over hills: serene blue clarity");
static const l10n_string_t PALETTE_NAME_CHUNGCHEONG_FIELD =
    L10N_ALL("chungcheong-field");
static const l10n_string_t PALETTE_DESC_CHUNGCHEONG_FIELD =
    L10N_ALL("Muted greens and dust gold of inland farmlands");
static const l10n_string_t PALETTE_NAME_JEJU_ROCK = L10N_ALL("jeju-rock");
static const l10n_string_t PALETTE_DESC_JEJU_ROCK =
    L10N_ALL("Volcanic basalt, moss green, and deep sea mist of Jeju Island");
static const l10n_string_t PALETTE_NAME_GYEONGSANGNAMDO =
    L10N_ALL("gyeongsangnamdo");
static const l10n_string_t PALETTE_DESC_GYEONGSANGNAMDO = L10N_ALL(
    "Sea breeze and industry -- blue steel, orange dusk, and vibrant harbors");
static const l10n_string_t PALETTE_NAME_BUSAN_HARBOR = L10N_ALL("busan-harbor");
static const l10n_string_t PALETTE_DESC_BUSAN_HARBOR =
    L10N_ALL("Night harbor lights and steel-blue waters of Busan Port");
static const l10n_string_t PALETTE_NAME_HAN = L10N_ALL("han");
static const l10n_string_t PALETTE_DESC_HAN = L10N_ALL(
    "Deep unresolved sorrow and austere beauty pale blue and gray layers");
static const l10n_string_t PALETTE_NAME_JEONG = L10N_ALL("jeong");
static const l10n_string_t PALETTE_DESC_JEONG =
    L10N_ALL("Warm emotional bonds and communal comfort soft red and gold "
             "glow on darkness");
static const l10n_string_t PALETTE_NAME_HEUNG = L10N_ALL("heung");
static const l10n_string_t PALETTE_DESC_HEUNG =
    L10N_ALL("Joyful energy and dynamic spirit: brilliant magenta and "
             "yellow over black");
static const l10n_string_t PALETTE_NAME_NUNCHI = L10N_ALL("nunchi");
static const l10n_string_t PALETTE_DESC_NUNCHI =
    L10N_ALL("Subtle perception and quiet adaptation: dim neutral tones "
             "with cyan glints");
static const l10n_string_t PALETTE_NAME_PCBANG_NIGHT = L10N_ALL("pcbang-night");
static const l10n_string_t PALETTE_DESC_PCBANG_NIGHT =
    L10N_ALL("Late-night gaming neon: cold blue LEDs, energy drink, and so on");
static const l10n_string_t PALETTE_NAME_ALCOHOL = L10N_ALL("alcohol");
static const l10n_string_t PALETTE_DESC_ALCOHOL = L10N_ALL(
    "Soju nights and neon haze: industrial green bottles and pink laughter");
static const l10n_string_t PALETTE_NAME_KOREAN_HARDCORE =
    L10N_ALL("korean-hardcore");
static const l10n_string_t PALETTE_DESC_KOREAN_HARDCORE = L10N_ALL(
    "I don't wanna die yet! neon blood and cold steel over asphalt black");
static const l10n_string_t PALETTE_NAME_KOREAN_NATIONALISTS =
    L10N_ALL("korean-nationalists");
static const l10n_string_t PALETTE_DESC_KOREAN_NATIONALISTS =
    L10N_ALL("Slightly exclusive types. you know the kind.");
static const l10n_string_t PALETTE_NAME_MEDIEVAL_KOREA =
    L10N_ALL("medieval-korea");
static const l10n_string_t PALETTE_DESC_MEDIEVAL_KOREA =
    L10N_ALL("Celadon grace and temple gold over aged ink-black lacquer");
static const l10n_string_t PALETTE_NAME_STONEAGE_KOREA =
    L10N_ALL("stoneage-korea");
static const l10n_string_t PALETTE_DESC_STONEAGE_KOREA =
    L10N_ALL("Primitive contrast of pale clothing and ground stone tools - "
             "raw earth and silence");
static const l10n_string_t PALETTE_NAME_FLAME_AND_BLOOD =
    L10N_ALL("flame-and-blood");
static const l10n_string_t PALETTE_DESC_FLAME_AND_BLOOD = L10N_ALL(
    "An East Asian war of 1592-1598. A great conflict akin to a world war, "
    "where flame met blood and nothing could be forsaken.");
static const l10n_string_t PALETTE_NAME_KOREAN_WAR = L10N_ALL("korean-war");
static const l10n_string_t PALETTE_DESC_KOREAN_WAR = L10N_ALL(
    "The Korean War: an unforgettable sorrow beneath ash, blood, and snow.");
static const l10n_string_t PALETTE_NAME_INDEPENDENCE_SPIRIT =
    L10N_ALL("independence-spirit");
static const l10n_string_t PALETTE_DESC_INDEPENDENCE_SPIRIT =
    L10N_ALL("The spirit of independence. A soul that we must remember.");

static const l10n_string_t PALETTE_NAME_USA_FLAG_256 = L10N_ALL("usa-flag-256");
static const l10n_string_t PALETTE_DESC_USA_FLAG_256 = L10N_ALL(
    "256-color reinterpretation of Flag blue base with red/white highlights");
static const l10n_string_t PALETTE_NAME_JPN_FLAG_256 = L10N_ALL("jpn-flag-256");
static const l10n_string_t PALETTE_DESC_JPN_FLAG_256 =
    L10N_ALL("256-color reinterpretation of Minimalist white with rising "
             "sun red accent");
static const l10n_string_t PALETTE_NAME_CHN_FLAG_256 = L10N_ALL("chn-flag-256");
static const l10n_string_t PALETTE_DESC_CHN_FLAG_256 = L10N_ALL(
    "256-color reinterpretation of Star-red background with lucky yellow text");
static const l10n_string_t PALETTE_NAME_RUS_FLAG_256 = L10N_ALL("rus-flag-256");
static const l10n_string_t PALETTE_DESC_RUS_FLAG_256 = L10N_ALL(
    "256-color reinterpretation of Tricolor base with strong red emphasis");
static const l10n_string_t PALETTE_NAME_DE_FLAG_256 = L10N_ALL("de-flag-256");
static const l10n_string_t PALETTE_DESC_DE_FLAG_256 =
    L10N_ALL("256-color reinterpretation of Tricolor base with strong "
             "red/yellow emphasis");
static const l10n_string_t PALETTE_NAME_HOLY_LIGHT_256 =
    L10N_ALL("holy-light-256");
static const l10n_string_t PALETTE_DESC_HOLY_LIGHT_256 =
    L10N_ALL("256-color reinterpretation of Christian sacred light on pure "
             "white/blue base");
static const l10n_string_t PALETTE_NAME_ISLAM_256 = L10N_ALL("islam-256");
static const l10n_string_t PALETTE_DESC_ISLAM_256 = L10N_ALL(
    "256-color reinterpretation of Iconic color of muslim, white/green base");
static const l10n_string_t PALETTE_NAME_DHARMA_OCHRE_256 =
    L10N_ALL("dharma-ochre-256");
static const l10n_string_t PALETTE_DESC_DHARMA_OCHRE_256 = L10N_ALL(
    "256-color reinterpretation of Ochre robes of enlightenment and vitality");
static const l10n_string_t PALETTE_NAME_YIN_YANG_256 = L10N_ALL("yin-yang-256");
static const l10n_string_t PALETTE_DESC_YIN_YANG_256 =
    L10N_ALL("256-color reinterpretation of Balance of Black and White with "
             "Jade accent");
static const l10n_string_t PALETTE_NAME_SOVIET_COLD_256 =
    L10N_ALL("soviet-cold-256");
static const l10n_string_t PALETTE_DESC_SOVIET_COLD_256 =
    L10N_ALL("256-color reinterpretation of Cold blue/white terminal for "
             "scientific systems");
static const l10n_string_t PALETTE_NAME_HI_TEL_256 = L10N_ALL("hi-tel-256");
static const l10n_string_t PALETTE_DESC_HI_TEL_256 =
    L10N_ALL("256-color reinterpretation of 1990s Korean BBS blue "
             "background and text style");
static const l10n_string_t PALETTE_NAME_AMIGA_CLI_256 =
    L10N_ALL("amiga-cli-256");
static const l10n_string_t PALETTE_DESC_AMIGA_CLI_256 =
    L10N_ALL("256-color reinterpretation of AmigaOS style with cyan/blue");
static const l10n_string_t PALETTE_NAME_JPN_PC98_256 = L10N_ALL("jpn-pc98-256");
static const l10n_string_t PALETTE_DESC_JPN_PC98_256 = L10N_ALL(
    "256-color reinterpretation of NEC PC-9801 subtle, earthy low-res tones");
static const l10n_string_t PALETTE_NAME_DEEP_BLUE_256 =
    L10N_ALL("deep-blue-256");
static const l10n_string_t PALETTE_DESC_DEEP_BLUE_256 =
    L10N_ALL("256-color reinterpretation of IBM Supercomputer monitoring "
             "interface style");
static const l10n_string_t PALETTE_NAME_KOREA_256 = L10N_ALL("korea-256");
static const l10n_string_t PALETTE_DESC_KOREA_256 =
    L10N_ALL("256-color reinterpretation of Taegeuk-gi inspired black base "
             "with red and blue accents");
static const l10n_string_t PALETTE_NAME_NEO_SEOUL_256 =
    L10N_ALL("neo-seoul-256");
static const l10n_string_t PALETTE_DESC_NEO_SEOUL_256 =
    L10N_ALL("256-color reinterpretation of Neon skyline of Gangnam and "
             "Hongdae: glowing magenta and cyan lights on dark asphalt");
static const l10n_string_t PALETTE_NAME_INCHEON_INDUSTRIAL_256 =
    L10N_ALL("incheon-industrial-256");
static const l10n_string_t PALETTE_DESC_INCHEON_INDUSTRIAL_256 =
    L10N_ALL("256-color reinterpretation of Metallic cranes and sodium "
             "streetlights of Incheon docks");
static const l10n_string_t PALETTE_NAME_GYEONGGI_MODERN_256 =
    L10N_ALL("gyeonggi-modern-256");
static const l10n_string_t PALETTE_DESC_GYEONGGI_MODERN_256 =
    L10N_ALL("256-color reinterpretation of Suburban calm of modern Korea. "
             "asphalt gray and warm window light");
static const l10n_string_t PALETTE_NAME_KOREAN_PALACE_256 =
    L10N_ALL("korean-palace-256");
static const l10n_string_t PALETTE_DESC_KOREAN_PALACE_256 =
    L10N_ALL("256-color reinterpretation of Royal dancheong harmony: jade "
             "green, vermilion red, and gold over black lacquer");
static const l10n_string_t PALETTE_NAME_GYEONGSANGBUKDO_256 =
    L10N_ALL("gyeongsangbukdo-256");
static const l10n_string_t PALETTE_DESC_GYEONGSANGBUKDO_256 = L10N_ALL(
    "256-color reinterpretation of Stoic mountains and agricultural spirit. "
    "stone, pine, and the quiet gold of temples");
static const l10n_string_t PALETTE_NAME_DAEGU_SUMMER_256 =
    L10N_ALL("daegu-summer-256");
static const l10n_string_t PALETTE_DESC_DAEGU_SUMMER_256 = L10N_ALL(
    "256-color reinterpretation of The biggest, the hottest of north "
    "gyeongsang: Blazing red-orange heat and festival gold under night sky");
static const l10n_string_t PALETTE_NAME_GYEONGJU_HERITAGE_256 =
    L10N_ALL("gyeongju-heritage-256");
static const l10n_string_t PALETTE_DESC_GYEONGJU_HERITAGE_256 = L10N_ALL(
    "256-color reinterpretation of Eternal relics and golden crowns: moonlit "
    "stone and ancient buddhism with blue flag of shilla military force");
static const l10n_string_t PALETTE_NAME_KANGWON_WINTER_256 =
    L10N_ALL("kangwon-winter-256");
static const l10n_string_t PALETTE_DESC_KANGWON_WINTER_256 =
    L10N_ALL("256-color reinterpretation of Cold white peaks and blue "
             "shadows of Gangwon's frozen dawn");
static const l10n_string_t PALETTE_NAME_ULSAN_STEEL_256 =
    L10N_ALL("ulsan-steel-256");
static const l10n_string_t PALETTE_DESC_ULSAN_STEEL_256 =
    L10N_ALL("256-color reinterpretation of Molten metal glow inside heavy "
             "industry furnace halls");
static const l10n_string_t PALETTE_NAME_JEOLLA_SEASIDE_256 =
    L10N_ALL("jeolla-seaside-256");
static const l10n_string_t PALETTE_DESC_JEOLLA_SEASIDE_256 =
    L10N_ALL("256-color reinterpretation of Quiet sea and horizon light of "
             "Mokpo and Yeosu nights");
static const l10n_string_t PALETTE_NAME_GWANGJU_BIENNALE_256 =
    L10N_ALL("gwangju-biennale-256");
static const l10n_string_t PALETTE_DESC_GWANGJU_BIENNALE_256 =
    L10N_ALL("256-color reinterpretation of Experimental art city with a "
             "heritage of democracy: violet neon and philosophical blue");
static const l10n_string_t PALETTE_NAME_JEONJU_HANOK_256 =
    L10N_ALL("jeonju-hanok-256");
static const l10n_string_t PALETTE_DESC_JEONJU_HANOK_256 =
    L10N_ALL("256-color reinterpretation of The symbol of north jeolla. "
             "warm roofs and calm golden light");
static const l10n_string_t PALETTE_NAME_DAEJEON_TECH_256 =
    L10N_ALL("daejeon-tech-256");
static const l10n_string_t PALETTE_DESC_DAEJEON_TECH_256 =
    L10N_ALL("256-color reinterpretation of Futuristic research district "
             "glow: clean LED light on steel gray night");
static const l10n_string_t PALETTE_NAME_SEJONG_NIGHT_256 =
    L10N_ALL("sejong-night-256");
static const l10n_string_t PALETTE_DESC_SEJONG_NIGHT_256 =
    L10N_ALL("256-color reinterpretation of Balanced dark-blue "
             "administration city under cool LED light");
static const l10n_string_t PALETTE_NAME_CHEONGJU_INTELLECT_256 =
    L10N_ALL("cheongju-intellect-256");
static const l10n_string_t PALETTE_DESC_CHEONGJU_INTELLECT_256 =
    L10N_ALL("256-color reinterpretation of Scholarly ink and soft dawn "
             "over hills: serene blue clarity");
static const l10n_string_t PALETTE_NAME_CHUNGCHEONG_FIELD_256 =
    L10N_ALL("chungcheong-field-256");
static const l10n_string_t PALETTE_DESC_CHUNGCHEONG_FIELD_256 =
    L10N_ALL("256-color reinterpretation of Muted greens and dust gold of "
             "inland farmlands");
static const l10n_string_t PALETTE_NAME_JEJU_ROCK_256 =
    L10N_ALL("jeju-rock-256");
static const l10n_string_t PALETTE_DESC_JEJU_ROCK_256 =
    L10N_ALL("256-color reinterpretation of Volcanic basalt, moss green, "
             "and deep sea mist of Jeju Island");
static const l10n_string_t PALETTE_NAME_GYEONGSANGNAMDO_256 =
    L10N_ALL("gyeongsangnamdo-256");
static const l10n_string_t PALETTE_DESC_GYEONGSANGNAMDO_256 =
    L10N_ALL("256-color reinterpretation of Sea breeze and industry -- blue "
             "steel, orange dusk, and vibrant harbors");
static const l10n_string_t PALETTE_NAME_BUSAN_HARBOR_256 =
    L10N_ALL("busan-harbor-256");
static const l10n_string_t PALETTE_DESC_BUSAN_HARBOR_256 =
    L10N_ALL("256-color reinterpretation of Night harbor lights and "
             "steel-blue waters of Busan Port");
static const l10n_string_t PALETTE_NAME_HAN_256 = L10N_ALL("han-256");
static const l10n_string_t PALETTE_DESC_HAN_256 =
    L10N_ALL("256-color reinterpretation of Deep unresolved sorrow and "
             "austere beauty pale blue and gray layers");
static const l10n_string_t PALETTE_NAME_JEONG_256 = L10N_ALL("jeong-256");
static const l10n_string_t PALETTE_DESC_JEONG_256 =
    L10N_ALL("256-color reinterpretation of Warm emotional bonds and "
             "communal comfort soft red and gold glow on darkness");
static const l10n_string_t PALETTE_NAME_HEUNG_256 = L10N_ALL("heung-256");
static const l10n_string_t PALETTE_DESC_HEUNG_256 =
    L10N_ALL("256-color reinterpretation of Joyful energy and dynamic "
             "spirit: brilliant magenta and yellow over black");
static const l10n_string_t PALETTE_NAME_NUNCHI_256 = L10N_ALL("nunchi-256");
static const l10n_string_t PALETTE_DESC_NUNCHI_256 =
    L10N_ALL("256-color reinterpretation of Subtle perception and quiet "
             "adaptation: dim neutral tones with blue glints");
static const l10n_string_t PALETTE_NAME_PCBANG_NIGHT_256 =
    L10N_ALL("pcbang-night-256");
static const l10n_string_t PALETTE_DESC_PCBANG_NIGHT_256 =
    L10N_ALL("256-color reinterpretation of Late-night gaming neon: cold "
             "blue LEDs, energy drink, and so on");
static const l10n_string_t PALETTE_NAME_ALCOHOL_256 = L10N_ALL("alcohol-256");
static const l10n_string_t PALETTE_DESC_ALCOHOL_256 =
    L10N_ALL("256-color reinterpretation of Soju nights and neon haze: "
             "industrial green bottles and pink laughter");
static const l10n_string_t PALETTE_NAME_KOREAN_HARDCORE_256 =
    L10N_ALL("korean-hardcore-256");
static const l10n_string_t PALETTE_DESC_KOREAN_HARDCORE_256 =
    L10N_ALL("256-color reinterpretation of I don't wanna die yet! neon "
             "blood and cold steel over asphalt black");
static const l10n_string_t PALETTE_NAME_KOREAN_NATIONALISTS_256 =
    L10N_ALL("korean-nationalists-256");
static const l10n_string_t PALETTE_DESC_KOREAN_NATIONALISTS_256 =
    L10N_ALL("256-color reinterpretation of Slightly exclusive types. you "
             "know the kind.");
static const l10n_string_t PALETTE_NAME_MEDIEVAL_KOREA_256 =
    L10N_ALL("medieval-korea-256");
static const l10n_string_t PALETTE_DESC_MEDIEVAL_KOREA_256 =
    L10N_ALL("256-color reinterpretation of Celadon grace and temple gold "
             "over aged ink-black lacquer");
static const l10n_string_t PALETTE_NAME_STONEAGE_KOREA_256 =
    L10N_ALL("stoneage-korea-256");
static const l10n_string_t PALETTE_DESC_STONEAGE_KOREA_256 =
    L10N_ALL("256-color reinterpretation of Primitive contrast of pale "
             "clothing and ground stone tools - raw earth and silence");
static const l10n_string_t PALETTE_NAME_FLAME_AND_BLOOD_256 =
    L10N_ALL("flame-and-blood-256");
static const l10n_string_t PALETTE_DESC_FLAME_AND_BLOOD_256 =
    L10N_ALL("256-color reinterpretation of An East Asian war of 1592-1598. "
             "A great conflict akin to a world war, where flame met blood "
             "and nothing could be forsaken.");
static const l10n_string_t PALETTE_NAME_KOREAN_WAR_256 =
    L10N_ALL("korean-war-256");
static const l10n_string_t PALETTE_DESC_KOREAN_WAR_256 =
    L10N_ALL("256-color reinterpretation of The Korean War: an "
             "unforgettable sorrow beneath ash, blood, and snow.");
static const l10n_string_t PALETTE_NAME_INDEPENDENCE_SPIRIT_256 =
    L10N_ALL("independence-spirit-256");
static const l10n_string_t PALETTE_DESC_INDEPENDENCE_SPIRIT_256 =
    L10N_ALL("256-color reinterpretation of The spirit of independence. A "
             "soul that we must remember.");

static const l10n_string_t PALETTE_NAME_FRENCH_ROCOCO_256 =
    L10N_ALL("french-rococo-256");
static const l10n_string_t PALETTE_DESC_FRENCH_ROCOCO_256 =
    L10N_ALL("Dramatic Silence--Rameau to Couperin");

static const l10n_string_t PALETTE_NAME_POLISH_CATHOLIC_256 =
    L10N_ALL("polish-catholic-256");
static const l10n_string_t PALETTE_DESC_POLISH_CATHOLIC_256 =
    L10N_ALL("Sacred Echoes--Chant to Solidarity");

static const l10n_string_t PALETTE_NAME_GERMAN_INDUSTRY_256 =
    L10N_ALL("german-industry-256");
static const l10n_string_t PALETTE_DESC_GERMAN_INDUSTRY_256 =
    L10N_ALL("Iron Will & Innovation--Ruhr to Raumfahrt");

static const l10n_string_t PALETTE_NAME_LEIPZIG_BLACK_256 =
    L10N_ALL("leipzig-black-256");
static const l10n_string_t PALETTE_DESC_LEIPZIG_BLACK_256 =
    L10N_ALL("Timeless Trendsetter--J.S Bach To Graffiti");

static const l10n_string_t PALETTE_NAME_CHOPIN_MAZURKA_256 =
    L10N_ALL("chopin-mazurka-256");
static const l10n_string_t PALETTE_DESC_CHOPIN_MAZURKA_256 =
    L10N_ALL("Poetic Melancholy--Salon to Soul");

static const palette_descriptor_t PALETTE_DEFINITIONS[] = {
    {"windows", &PALETTE_NAME_WINDOWS, &PALETTE_DESC_WINDOWS, "cyan", "blue",
     true, "white", "blue", "yellow", true, false},
    {"gnu-linux", &PALETTE_NAME_GNU_LINUX, &PALETTE_DESC_GNU_LINUX,
     "bright-green", "black", true, "blue", "black", "bright-yellow", true,
     false},
    {"macos", &PALETTE_NAME_MACOS, &PALETTE_DESC_MACOS, "bright-white", "black",
     false, "bright-blue", "black", "white", false, false},
    {"freebsd", &PALETTE_NAME_FREEBSD, &PALETTE_DESC_FREEBSD, "bright-red",
     "black", false, "red", "black", "bright-white", false, false},
    {"solaris", &PALETTE_NAME_SOLARIS, &PALETTE_DESC_SOLARIS, "bright-yellow",
     "black", true, "bright-red", "black", "bright-white", true, false},
    {"openbsd-fortress", &PALETTE_NAME_OPENBSD_FORTRESS,
     &PALETTE_DESC_OPENBSD_FORTRESS, "bright-blue", "black", false,
     "bright-white", "black", "cyan", false, false},
    {"netbsd-universal", &PALETTE_NAME_NETBSD_UNIVERSAL,
     &PALETTE_DESC_NETBSD_UNIVERSAL, "bright-cyan", "black", false,
     "bright-white", "black", "bright-yellow", false, false},
    {"moe", &PALETTE_NAME_MOE, &PALETTE_DESC_MOE, "white", "magenta",
     true, "white", "magenta", "cyan", true, false},
    {"neon-genesis-evangelion", &PALETTE_NAME_NEON_GENESIS_EVANGELION,
     &PALETTE_DESC_NEON_GENESIS_EVANGELION, "black", "white", true,
     "white", "magenta", "green", true, false},
    {"megami", &PALETTE_NAME_MEGAMI, &PALETTE_DESC_MEGAMI, "bright-white",
     "black", false, "bright-yellow", "blue", "cyan", false, false},
    {"clean", &PALETTE_NAME_CLEAN, &PALETTE_DESC_CLEAN, "default", "default",
     false, "white", "default", "default", false, false},
    {"adwaita", &PALETTE_NAME_ADWAITA, &PALETTE_DESC_ADWAITA, "blue", "default",
     false, "blue", "bright-white", "white", true, false},
    {"80shacker", &PALETTE_NAME_80SHACKER, &PALETTE_DESC_80SHACKER,
     "bright-green", "default", true, "bright-green", "default", "default",
     true, false},
    {"plato", &PALETTE_NAME_PLATO, &PALETTE_DESC_PLATO, "yellow", "default",
     false, "yellow", "default", "default", false, false},
    {"atarist", &PALETTE_NAME_ATARIST, &PALETTE_DESC_ATARIST, "bright-white",
     "black", true, "bright-white", "black", "yellow", false, false},
    {"win95bsod", &PALETTE_NAME_WIN95BSOD, &PALETTE_DESC_WIN95BSOD,
     "bright-white", "blue", true, "bright-white", "blue", "cyan", true, false},
    {"chn-hanzi", &PALETTE_NAME_CHN_HANZI, &PALETTE_DESC_CHN_HANZI,
     "bright-cyan", "black", true, "white", "black", "cyan", true, false},
    {"usa-flag", &PALETTE_NAME_USA_FLAG, &PALETTE_DESC_USA_FLAG, "bright-white",
     "black", true, "blue", "yellow", "red", true, false},
    {"jpn-flag", &PALETTE_NAME_JPN_FLAG, &PALETTE_DESC_JPN_FLAG, "bright-white",
     "black", false, "white", "black", "red", true, false},
    {"chn-flag", &PALETTE_NAME_CHN_FLAG, &PALETTE_DESC_CHN_FLAG,
     "bright-white", "red", true, "yellow", "red", "white", true, false},
    {"rus-flag", &PALETTE_NAME_RUS_FLAG, &PALETTE_DESC_RUS_FLAG, "bright-white",
     "black", true, "blue", "red", "bright-white", true, false},
    {"de-flag", &PALETTE_NAME_DE_FLAG, &PALETTE_DESC_DE_FLAG, "yellow",
     "black", true, "yellow", "black", "red", true, false},
    {"holy-light", &PALETTE_NAME_HOLY_LIGHT, &PALETTE_DESC_HOLY_LIGHT,
     "bright-white", "blue", false, "blue", "black", "yellow", true, false},
    {"islam", &PALETTE_NAME_ISLAM, &PALETTE_DESC_ISLAM, "bright-white", "green",
     false, "green", "black", "bright-white", true, false},
    {"dharma-ochre", &PALETTE_NAME_DHARMA_OCHRE, &PALETTE_DESC_DHARMA_OCHRE,
     "yellow", "black", true, "red", "black", "yellow", true, false},
    {"yin-yang", &PALETTE_NAME_YIN_YANG, &PALETTE_DESC_YIN_YANG, "white",
     "black", false, "bright-green", "black", "white", false, false},
    {"soviet-cold", &PALETTE_NAME_SOVIET_COLD, &PALETTE_DESC_SOVIET_COLD,
     "white", "blue", false, "white", "blue", "bright-yellow", true, false},
    {"soviet-cold-256", &PALETTE_NAME_SOVIET_COLD_256,
     &PALETTE_DESC_SOVIET_COLD_256, "xterm:231", "xterm-bg:19", false,
     "xterm:231", "xterm-bg:19", "xterm-bg:220", true, true},
    {"hi-tel", &PALETTE_NAME_HI_TEL, &PALETTE_DESC_HI_TEL, "bright-white",
     "blue", true, "bright-white", "blue", "magenta", true, false},
    {"blackpink", &PALETTE_NAME_BLACKPINK, &PALETTE_DESC_BLACKPINK,
     "bright-white", "blue", false, "bright-white", "black", "magenta", false,
     false},
    {"amiga-cli", &PALETTE_NAME_AMIGA_CLI, &PALETTE_DESC_AMIGA_CLI, "cyan",
     "blue", true, "white", "blue", "blue", true, false},
    {"jpn-pc98", &PALETTE_NAME_JPN_PC98, &PALETTE_DESC_JPN_PC98, "yellow",
     "black", false, "red", "black", "yellow", false, false},
    {"deep-blue", &PALETTE_NAME_DEEP_BLUE, &PALETTE_DESC_DEEP_BLUE, "white",
     "blue", true, "cyan", "blue", "white", true, false},
    {"korea", &PALETTE_NAME_KOREA, &PALETTE_DESC_KOREA, "black", "blue",
     true, "bright-white", "blue", "red", true, false},
    {"neo-seoul", &PALETTE_NAME_NEO_SEOUL, &PALETTE_DESC_NEO_SEOUL,
     "bright-magenta", "black", true, "bright-cyan", "black", "cyan", true,
     false},
    {"incheon-industrial", &PALETTE_NAME_INCHEON_INDUSTRIAL,
     &PALETTE_DESC_INCHEON_INDUSTRIAL, "bright-yellow", "black", true,
     "bright-yellow", "black", "bright-red", true, false},
    {"gyeonggi-modern", &PALETTE_NAME_GYEONGGI_MODERN,
     &PALETTE_DESC_GYEONGGI_MODERN, "bright-white", "black", false,
     "bright-yellow", "black", "bright-cyan", false, false},
    {"korean-palace", &PALETTE_NAME_KOREAN_PALACE, &PALETTE_DESC_KOREAN_PALACE,
     "bright-yellow", "black", true, "red", "black", "green", false, false},
    {"gyeongsangbukdo", &PALETTE_NAME_GYEONGSANGBUKDO,
     &PALETTE_DESC_GYEONGSANGBUKDO, "bright-yellow", "black", false,
     "bright-green", "black", "bright-white", false, false},
    {"daegu-summer", &PALETTE_NAME_DAEGU_SUMMER, &PALETTE_DESC_DAEGU_SUMMER,
     "yellow", "black", true, "yellow", "black", "cyan", true,
     false},
    {"gyeongju-heritage", &PALETTE_NAME_GYEONGJU_HERITAGE,
     &PALETTE_DESC_GYEONGJU_HERITAGE, "bright-white", "black", false,
     "bright-yellow", "black", "blue", false, false},
    {"kangwon-winter", &PALETTE_NAME_KANGWON_WINTER,
     &PALETTE_DESC_KANGWON_WINTER, "bright-white", "blue", true, "bright-cyan",
     "blue", "white", true, false},
    {"ulsan-steel", &PALETTE_NAME_ULSAN_STEEL, &PALETTE_DESC_ULSAN_STEEL,
     "bright-red", "black", true, "bright-yellow", "black", "red", true, false},
    {"jeolla-seaside", &PALETTE_NAME_JEOLLA_SEASIDE,
     &PALETTE_DESC_JEOLLA_SEASIDE, "bright-cyan", "black", false, "cyan",
     "black", "bright-blue", true, false},
    {"gwangju-biennale", &PALETTE_NAME_GWANGJU_BIENNALE,
     &PALETTE_DESC_GWANGJU_BIENNALE, "bright-magenta", "black", true,
     "bright-blue", "black", "magenta", true, false},
    {"jeonju-hanok", &PALETTE_NAME_JEONJU_HANOK, &PALETTE_DESC_JEONJU_HANOK,
     "bright-yellow", "black", false, "yellow", "black", "bright-white", false,
     false},
    {"daejeon-tech", &PALETTE_NAME_DAEJEON_TECH, &PALETTE_DESC_DAEJEON_TECH,
     "white", "black", true, "white", "black", "bright-green", true, false},
    {"sejong-night", &PALETTE_NAME_SEJONG_NIGHT, &PALETTE_DESC_SEJONG_NIGHT,
     "bright-white", "blue", true, "bright-cyan", "blue", "white", true, false},
    {"cheongju-intellect", &PALETTE_NAME_CHEONGJU_INTELLECT,
     &PALETTE_DESC_CHEONGJU_INTELLECT, "bright-cyan", "black", false,
     "bright-white", "black", "cyan", false, false},
    {"chungcheong-field", &PALETTE_NAME_CHUNGCHEONG_FIELD,
     &PALETTE_DESC_CHUNGCHEONG_FIELD, "yellow", "black", false, "green",
     "black", "yellow", false, false},
    {"jeju-rock", &PALETTE_NAME_JEJU_ROCK, &PALETTE_DESC_JEJU_ROCK,
     "bright-green", "black", false, "bright-cyan", "black", "green", false,
     false},
    {"gyeongsangnamdo", &PALETTE_NAME_GYEONGSANGNAMDO,
     &PALETTE_DESC_GYEONGSANGNAMDO, "bright-blue", "black", true,
     "bright-yellow", "black", "bright-cyan", true, false},
    {"busan-harbor", &PALETTE_NAME_BUSAN_HARBOR, &PALETTE_DESC_BUSAN_HARBOR,
     "bright-blue", "black", true, "cyan", "black", "bright-blue", true, false},
    {"han", &PALETTE_NAME_HAN, &PALETTE_DESC_HAN, "bright-cyan", "blue", false,
     "white", "blue", "black", false, false},
    {"jeong", &PALETTE_NAME_JEONG, &PALETTE_DESC_JEONG, "bright-red", "black",
     true, "white", "black", "bright-yellow", true, false},
    {"heung", &PALETTE_NAME_HEUNG, &PALETTE_DESC_HEUNG, "bright-magenta",
     "black", true, "bright-yellow", "black", "magenta", true, false},
    {"nunchi", &PALETTE_NAME_NUNCHI, &PALETTE_DESC_NUNCHI, "white", "black",
     false, "bright-cyan", "black", "cyan", false, false},
    {"pcbang-night", &PALETTE_NAME_PCBANG_NIGHT, &PALETTE_DESC_PCBANG_NIGHT,
     "bright-cyan", "black", true, "bright-red", "black", "bright-blue", true,
     false},
    {"alcohol", &PALETTE_NAME_ALCOHOL, &PALETTE_DESC_ALCOHOL, "bright-green",
     "black", true, "bright-magenta", "black", "green", true, false},
    {"korean-hardcore", &PALETTE_NAME_KOREAN_HARDCORE,
     &PALETTE_DESC_KOREAN_HARDCORE, "bright-red", "black", true, "bright-blue",
     "black", "bright-red", true, false},
    {"korean-nationalists", &PALETTE_NAME_KOREAN_NATIONALISTS,
     &PALETTE_DESC_KOREAN_NATIONALISTS, "bright-green", "black", true,
     "bright-blue", "black", "bright-cyan", true, false},
    {"medieval-korea", &PALETTE_NAME_MEDIEVAL_KOREA,
     &PALETTE_DESC_MEDIEVAL_KOREA, "bright-cyan", "black", false,
     "bright-yellow", "black", "cyan", false, false},
    {"stoneage-korea", &PALETTE_NAME_STONEAGE_KOREA,
     &PALETTE_DESC_STONEAGE_KOREA, "bright-white", "black", false,
     "bright-yellow", "black", "white", false, false},
    {"flame-and-blood", &PALETTE_NAME_FLAME_AND_BLOOD,
     &PALETTE_DESC_FLAME_AND_BLOOD, "bright-blue", "black", true,
     "bright-yellow", "black", "red", true, false},
    {"korean-war", &PALETTE_NAME_KOREAN_WAR, &PALETTE_DESC_KOREAN_WAR,
     "bright-white", "black", false, "bright-red", "black", "white", false,
     false},
    {"independence-spirit", &PALETTE_NAME_INDEPENDENCE_SPIRIT,
     &PALETTE_DESC_INDEPENDENCE_SPIRIT, "bright-red", "black", true, "blue",
     "black", "bright-yellow", true, false},
    {"usa-flag-256", &PALETTE_NAME_USA_FLAG_256, &PALETTE_DESC_USA_FLAG_256,
     "xterm:231", "xterm-bg:20", true, "xterm:196", "xterm-bg:20",
     "xterm-bg:231", true, true},
    {"jpn-flag-256", &PALETTE_NAME_JPN_FLAG_256, &PALETTE_DESC_JPN_FLAG_256,
     "xterm:231", "xterm-bg:16", false, "xterm:196", "xterm-bg:16",
     "xterm-bg:231", true, true},
    {"chn-flag-256", &PALETTE_NAME_CHN_FLAG_256, &PALETTE_DESC_CHN_FLAG_256,
     "xterm:220", "xterm-bg:88", true, "xterm:220", "xterm-bg:88",
     "xterm-bg:15", false, false},
    {"rus-flag-256", &PALETTE_NAME_RUS_FLAG_256, &PALETTE_DESC_RUS_FLAG_256,
     "xterm:231", "xterm-bg:20", true, "xterm:196", "xterm-bg:20",
     "xterm-bg:231", true, true},
    {"de-flag-256", &PALETTE_NAME_DE_FLAG_256, &PALETTE_DESC_DE_FLAG_256,
     "xterm:238", "xterm-bg:16", true, "xterm:226", "xterm-bg:16",
     "xterm-bg:196", true, true},
    {"holy-light-256", &PALETTE_NAME_HOLY_LIGHT_256,
     &PALETTE_DESC_HOLY_LIGHT_256, "xterm:231", "xterm-bg:45", false,
     "xterm:45", "xterm-bg:16", "xterm-bg:226", true, true},
    {"islam-256", &PALETTE_NAME_ISLAM_256, &PALETTE_DESC_ISLAM_256, "xterm:231",
     "xterm-bg:34", false, "xterm:34", "xterm-bg:16", "xterm-bg:231", true,
     true},
    {"dharma-ochre-256", &PALETTE_NAME_DHARMA_OCHRE_256,
     &PALETTE_DESC_DHARMA_OCHRE_256, "xterm:226", "xterm-bg:16", true,
     "xterm:196", "xterm-bg:16", "xterm-bg:226", true, true},
    {"yin-yang-256", &PALETTE_NAME_YIN_YANG_256, &PALETTE_DESC_YIN_YANG_256,
     "xterm:15", "xterm-bg:16", false, "xterm:34", "xterm-bg:16", "xterm-bg:15",
     false, true},
    {"hi-tel-256", &PALETTE_NAME_HI_TEL_256, &PALETTE_DESC_HI_TEL_256,
     "xterm:231", "xterm-bg:20", true, "xterm:231", "xterm-bg:20",
     "xterm-bg:165", true, true},
    {"amiga-cli-256", &PALETTE_NAME_AMIGA_CLI_256, &PALETTE_DESC_AMIGA_CLI_256,
     "xterm:51", "xterm-bg:20", true, "xterm:51", "xterm-bg:20", "xterm-bg:231",
     true, true},
    {"jpn-pc98-256", &PALETTE_NAME_JPN_PC98_256, &PALETTE_DESC_JPN_PC98_256,
     "xterm:226", "xterm-bg:16", false, "xterm:196", "xterm-bg:16",
     "xterm-bg:226", false, true},
    {"deep-blue-256", &PALETTE_NAME_DEEP_BLUE_256, &PALETTE_DESC_DEEP_BLUE_256,
     "xterm:15", "xterm-bg:20", true, "xterm:51", "xterm-bg:20", "xterm-bg:15",
     true, true},
    {"korea-256", &PALETTE_NAME_KOREA_256, &PALETTE_DESC_KOREA_256, "xterm:81",
     "xterm-bg:20", true, "xterm:231", "xterm-bg:20", "xterm-bg:196", true,
     true},
    {"neo-seoul-256", &PALETTE_NAME_NEO_SEOUL_256, &PALETTE_DESC_NEO_SEOUL_256,
     "xterm:207", "xterm-bg:16", true, "xterm:117", "xterm-bg:16",
     "xterm-bg:229", true, true},
    {"incheon-industrial-256", &PALETTE_NAME_INCHEON_INDUSTRIAL_256,
     &PALETTE_DESC_INCHEON_INDUSTRIAL_256, "xterm:229", "xterm-bg:16", true,
     "xterm:229", "xterm-bg:16", "xterm-bg:220", true, true},
    {"gyeonggi-modern-256", &PALETTE_NAME_GYEONGGI_MODERN_256,
     &PALETTE_DESC_GYEONGGI_MODERN_256, "xterm:231", "xterm-bg:16", false,
     "xterm:229", "xterm-bg:16", "xterm-bg:72", false, true},
    {"korean-palace-256", &PALETTE_NAME_KOREAN_PALACE_256,
     &PALETTE_DESC_KOREAN_PALACE_256, "xterm:229", "xterm-bg:16", true,
     "xterm:131", "xterm-bg:16", "xterm-bg:65", false, true},
    {"gyeongsangbukdo-256", &PALETTE_NAME_GYEONGSANGBUKDO_256,
     &PALETTE_DESC_GYEONGSANGBUKDO_256, "xterm:229", "xterm-bg:16", false,
     "xterm:118", "xterm-bg:16", "xterm-bg:231", false, true},
    {"daegu-summer-256", &PALETTE_NAME_DAEGU_SUMMER_256,
     &PALETTE_DESC_DAEGU_SUMMER_256, "xterm:203", "xterm-bg:16", true,
     "xterm:229", "xterm-bg:16", "xterm-bg:75", true, true},
    {"gyeongju-heritage-256", &PALETTE_NAME_GYEONGJU_HERITAGE_256,
     &PALETTE_DESC_GYEONGJU_HERITAGE_256, "xterm:231", "xterm-bg:16", false,
     "xterm:229", "xterm-bg:16", "xterm-bg:20", false, true},
    {"kangwon-winter-256", &PALETTE_NAME_KANGWON_WINTER_256,
     &PALETTE_DESC_KANGWON_WINTER_256, "xterm:231", "xterm-bg:20", true,
     "xterm:123", "xterm-bg:20", "xterm-bg:15", true, true},
    {"ulsan-steel-256", &PALETTE_NAME_ULSAN_STEEL_256,
     &PALETTE_DESC_ULSAN_STEEL_256, "xterm:203", "xterm-bg:16", true,
     "xterm:229", "xterm-bg:16", "xterm-bg:208", true, true},
    {"jeolla-seaside-256", &PALETTE_NAME_JEOLLA_SEASIDE_256,
     &PALETTE_DESC_JEOLLA_SEASIDE_256, "xterm:95", "xterm-bg:16", false,
     "xterm:51", "xterm-bg:16", "xterm-bg:95", true, true},
    {"gwangju-biennale-256", &PALETTE_NAME_GWANGJU_BIENNALE_256,
     &PALETTE_DESC_GWANGJU_BIENNALE_256, "xterm:207", "xterm-bg:16", true,
     "xterm:81", "xterm-bg:16", "xterm-bg:165", true, true},
    {"jeonju-hanok-256", &PALETTE_NAME_JEONJU_HANOK_256,
     &PALETTE_DESC_JEONJU_HANOK_256, "xterm:231", "xterm-bg:16", false,
     "xterm:226", "xterm-bg:16", "xterm-bg:1", false, true},
    {"daejeon-tech-256", &PALETTE_NAME_DAEJEON_TECH_256,
     &PALETTE_DESC_DAEJEON_TECH_256, "xterm:15", "xterm-bg:16", true,
     "xterm:15", "xterm-bg:16", "xterm-bg:64", true, true},
    {"sejong-night-256", &PALETTE_NAME_SEJONG_NIGHT_256,
     &PALETTE_DESC_SEJONG_NIGHT_256, "xterm:231", "xterm-bg:20", true,
     "xterm:90", "xterm-bg:20", "xterm-bg:15", true, true},
    {"cheongju-intellect-256", &PALETTE_NAME_CHEONGJU_INTELLECT_256,
     &PALETTE_DESC_CHEONGJU_INTELLECT_256, "xterm:123", "xterm-bg:16", false,
     "xterm:178", "xterm-bg:16", "xterm-bg:123", false, true},
    {"chungcheong-field-256", &PALETTE_NAME_CHUNGCHEONG_FIELD_256,
     &PALETTE_DESC_CHUNGCHEONG_FIELD_256, "xterm:226", "xterm-bg:16", false,
     "xterm:34", "xterm-bg:16", "xterm-bg:226", false, true},
    {"jeju-rock-256", &PALETTE_NAME_JEJU_ROCK_256, &PALETTE_DESC_JEJU_ROCK_256,
     "xterm:118", "xterm-bg:16", false, "xterm:123", "xterm-bg:16",
     "xterm-bg:34", false, true},
    {"gyeongsangnamdo-256", &PALETTE_NAME_GYEONGSANGNAMDO_256,
     &PALETTE_DESC_GYEONGSANGNAMDO_256, "xterm:81", "xterm-bg:16", true,
     "xterm:229", "xterm-bg:16", "xterm-bg:123", true, true},
    {"busan-harbor-256", &PALETTE_NAME_BUSAN_HARBOR_256,
     &PALETTE_DESC_BUSAN_HARBOR_256, "xterm:81", "xterm-bg:16", true,
     "xterm:255", "xterm-bg:16", "xterm-bg:81", true, true},
    {"han-256", &PALETTE_NAME_HAN_256, &PALETTE_DESC_HAN_256, "xterm:123",
     "xterm-bg:75", false, "xterm:246", "xterm-bg:20", "xterm-bg:254", false,
     true},
    {"jeong-256", &PALETTE_NAME_JEONG_256, &PALETTE_DESC_JEONG_256, "xterm:203",
     "xterm-bg:16", true, "xterm:203", "xterm-bg:16", "xterm-bg:220", true,
     true},
    {"heung-256", &PALETTE_NAME_HEUNG_256, &PALETTE_DESC_HEUNG_256, "xterm:207",
     "xterm-bg:16", true, "xterm:229", "xterm-bg:16", "xterm-bg:165", true,
     true},
    {"nunchi-256", &PALETTE_NAME_NUNCHI_256, &PALETTE_DESC_NUNCHI_256,
     "xterm:15", "xterm-bg:16", false, "xterm:105", "xterm-bg:16",
     "xterm-bg:15", false, true},
    {"pcbang-night-256", &PALETTE_NAME_PCBANG_NIGHT_256,
     &PALETTE_DESC_PCBANG_NIGHT_256, "xterm:123", "xterm-bg:16", true,
     "xterm:203", "xterm-bg:16", "xterm-bg:220", true, true},
    {"alcohol-256", &PALETTE_NAME_ALCOHOL_256, &PALETTE_DESC_ALCOHOL_256,
     "xterm:118", "xterm-bg:16", true, "xterm:207", "xterm-bg:16",
     "xterm-bg:34", true, true},
    {"korean-hardcore-256", &PALETTE_NAME_KOREAN_HARDCORE_256,
     &PALETTE_DESC_KOREAN_HARDCORE_256, "xterm:203", "xterm-bg:16", true,
     "xterm:81", "xterm-bg:16", "xterm-bg:220", true, true},
    {"korean-nationalists-256", &PALETTE_NAME_KOREAN_NATIONALISTS_256,
     &PALETTE_DESC_KOREAN_NATIONALISTS_256, "xterm:118", "xterm-bg:16", true,
     "xterm:81", "xterm-bg:16", "xterm-bg:203", true, true},
    {"medieval-korea-256", &PALETTE_NAME_MEDIEVAL_KOREA_256,
     &PALETTE_DESC_MEDIEVAL_KOREA_256, "xterm:30", "xterm-bg:16", false,
     "xterm:229", "xterm-bg:16", "xterm-bg:30", false, true},
    {"stoneage-korea-256", &PALETTE_NAME_STONEAGE_KOREA_256,
     &PALETTE_DESC_STONEAGE_KOREA_256, "xterm:231", "xterm-bg:16", false,
     "xterm:235", "xterm-bg:229", "xterm-bg:94", false, true},
    {"flame-and-blood-256", &PALETTE_NAME_FLAME_AND_BLOOD_256,
     &PALETTE_DESC_FLAME_AND_BLOOD_256, "xterm:81", "xterm-bg:16", true,
     "xterm:229", "xterm-bg:16", "xterm-bg:196", true, true},
    {"korean-war-256", &PALETTE_NAME_KOREAN_WAR_256,
     &PALETTE_DESC_KOREAN_WAR_256, "xterm:231", "xterm-bg:16", false,
     "xterm:203", "xterm-bg:16", "xterm-bg:15", false, true},
    {"independence-spirit-256", &PALETTE_NAME_INDEPENDENCE_SPIRIT_256,
     &PALETTE_DESC_INDEPENDENCE_SPIRIT_256, "xterm:203", "xterm-bg:16", true,
     "xterm:45", "xterm-bg:16", "xterm-bg:229", true, true},
    {"monokai", &PALETTE_NAME_MONOKAI, &PALETTE_DESC_MONOKAI, "xterm:118",
     "xterm-bg:239", false, "xterm:255", "xterm-bg:235", "xterm-bg:239", false,
     true},
    {"french-rococo-256", &PALETTE_NAME_FRENCH_ROCOCO_256,
     &PALETTE_DESC_FRENCH_ROCOCO_256, "xterm:218", "xterm-bg:229", false,
     "xterm:209", "xterm-bg:231", "xterm-bg:147", false, true},
    {"polish-catholic-256", &PALETTE_NAME_POLISH_CATHOLIC_256,
     &PALETTE_DESC_POLISH_CATHOLIC_256, "xterm:226", "xterm-bg:21", true,
     "xterm:231", "xterm-bg:16", "xterm-bg:196", true, true},
    {"german-industry-256", &PALETTE_NAME_GERMAN_INDUSTRY_256,
     &PALETTE_DESC_GERMAN_INDUSTRY_256, "xterm:208", "xterm-bg:238", true,
     "xterm:253", "xterm-bg:235", "xterm-bg:220", true, true},
    {"leipzig-black-256", &PALETTE_NAME_LEIPZIG_BLACK_256,
     &PALETTE_DESC_LEIPZIG_BLACK_256, "xterm:255", "xterm-bg:232", false,
     "xterm:255", "xterm-bg:16", "xterm-bg:240", false, true},
    {"chopin-mazurka-256", &PALETTE_NAME_CHOPIN_MAZURKA_256,
     &PALETTE_DESC_CHOPIN_MAZURKA_256, "xterm:111", "xterm-bg:16", false,
     "xterm:253", "xterm-bg:233", "xterm-bg:176", false, true},
};

typedef int (*accept_channel_fn_t)(ssh_message, ssh_channel);

#if defined(__GNUC__)
extern int ssh_message_channel_request_open_reply_accept_channel(
    ssh_message message, ssh_channel channel) __attribute__((weak));
#endif

static void resolve_accept_channel_once(void);
static accept_channel_fn_t g_accept_channel_fn = nullptr;
static pthread_once_t g_accept_channel_once = PTHREAD_ONCE_INIT;

static accept_channel_fn_t resolve_accept_channel_fn(void)
{
    pthread_once(&g_accept_channel_once, resolve_accept_channel_once);
    return g_accept_channel_fn;
}

static void resolve_accept_channel_once(void)
{
#if defined(__GNUC__)
    if (ssh_message_channel_request_open_reply_accept_channel != nullptr) {
        g_accept_channel_fn =
            ssh_message_channel_request_open_reply_accept_channel;
        return;
    }
#endif

    static const char *kSymbol =
        "ssh_message_channel_request_open_reply_accept_channel";

#if defined(RTLD_DEFAULT)
    g_accept_channel_fn = (accept_channel_fn_t)dlsym(RTLD_DEFAULT, kSymbol);
    if (g_accept_channel_fn != nullptr) {
        return;
    }
#endif

    const char *candidates[] = {"libssh.so.4", "libssh.so", "libssh.dylib"};
    for (size_t idx = 0; idx < sizeof(candidates) / sizeof(candidates[0]);
         ++idx) {
        const char *name = candidates[idx];
        void *handle = dlopen(name, RTLD_LAZY | RTLD_LOCAL);
        if (handle == nullptr) {
            handle = dlopen(name, RTLD_LAZY);
        }
        if (handle == nullptr) {
            continue;
        }

        g_accept_channel_fn = (accept_channel_fn_t)dlsym(handle, kSymbol);
        if (g_accept_channel_fn != nullptr) {
            return;
        }
    }
}

void trim_whitespace_inplace(char *text);
static const char *lookup_color_code(const color_entry_t *entries,
                                     size_t entry_count, const char *name);
static bool parse_bool_token(const char *token, bool *value);
static bool session_transport_active(const session_ctx_t *ctx);
static void session_transport_request_close(session_ctx_t *ctx);
void session_channel_write(session_ctx_t *ctx, const void *data,
                                  size_t length);
static bool session_channel_write_cp437(session_ctx_t *ctx, const char *data,
                                        size_t length);
static bool session_channel_write_utf16(session_ctx_t *ctx, const char *data,
                                        size_t length);
static bool session_channel_write_utf16_segment(session_ctx_t *ctx,
                                                const char *data,
                                                size_t length);
static size_t session_utf8_decode_codepoint(const unsigned char *data,
                                            size_t length, uint32_t *codepoint);
static bool session_utf8_to_utf16le(const char *input, size_t length,
                                    unsigned char *output, size_t capacity,
                                    size_t *produced);
static bool session_channel_write_all(session_ctx_t *ctx, const void *data,
                                      size_t length);
static bool session_output_lock(session_ctx_t *ctx);
static void session_output_unlock(session_ctx_t *ctx);
static bool session_channel_wait_writable(session_ctx_t *ctx, int timeout_ms);
static void session_channel_log_write_failure(session_ctx_t *ctx,
                                              const char *reason);
static void session_channel_flush(session_ctx_t *ctx);
static void session_output_buffer_flush(session_ctx_t *ctx);
static int session_transport_read(session_ctx_t *ctx, void *buffer,
                                  size_t length, int timeout_ms);
static bool session_transport_is_open(const session_ctx_t *ctx);
static bool session_transport_is_eof(const session_ctx_t *ctx);
static void session_apply_background_fill(session_ctx_t *ctx);
static void session_write_rendered_line(session_ctx_t *ctx,
                                        const char *render_source);
static void session_send_caption_line(session_ctx_t *ctx, const char *message);
static void session_render_caption_with_offset(session_ctx_t *ctx,
                                               const char *message,
                                               size_t move_up);
static void session_send_line(session_ctx_t *ctx, const char *message);
static void session_send_plain_line(session_ctx_t *ctx, const char *message);
static void session_send_multiline_message(session_ctx_t *ctx,
                                           const char *message);
void session_send_raw_text(session_ctx_t *ctx, const char *text);

static void session_render_banner(session_ctx_t *ctx);
static const char *session_editor_terminator(const session_ctx_t *ctx);
static bool session_editor_matches_terminator(const session_ctx_t *ctx,
                                              const char *line);
static size_t session_editor_body_capacity(const session_ctx_t *ctx);
static size_t session_editor_max_lines(const session_ctx_t *ctx);
static void session_format_separator_line(session_ctx_t *ctx, const char *label,
                                          char *out, size_t length);
static void session_render_separator(session_ctx_t *ctx, const char *label);
static void session_clear_screen(session_ctx_t *ctx);
static void session_bbs_prepare_canvas(session_ctx_t *ctx);
static void session_bbs_render_editor(session_ctx_t *ctx, const char *status);
static void session_bbs_recalculate_line_count(session_ctx_t *ctx);
static bool session_bbs_get_line_range(const session_ctx_t *ctx,
                                       size_t line_index, size_t *start,
                                       size_t *length);
static void session_bbs_copy_line(const session_ctx_t *ctx, size_t line_index,
                                  char *buffer, size_t length);
static bool session_bbs_append_line(session_ctx_t *ctx, const char *line,
                                    char *status, size_t status_length);
static bool session_bbs_replace_line(session_ctx_t *ctx, size_t line_index,
                                     const char *line, char *status,
                                     size_t status_length);
static void session_bbs_commit_edit_in_place(session_ctx_t *ctx);
static void session_bbs_move_cursor(session_ctx_t *ctx, int direction);
static bool session_bbs_is_admin_only_tag(const char *tag);
static void session_bbs_buffer_breaking_notice(session_ctx_t *ctx,
                                               const char *message);
static bool session_bbs_should_defer_breaking(session_ctx_t *ctx,
                                              const char *message);
static void session_render_prompt(session_ctx_t *ctx, bool include_separator);
static void session_refresh_input_line(session_ctx_t *ctx);
static void session_set_input_text(session_ctx_t *ctx, const char *text);
static void session_local_echo_char(session_ctx_t *ctx, char ch);
static void session_local_backspace(session_ctx_t *ctx);
static void session_clear_input(session_ctx_t *ctx);
static void session_clear_input_without_prompt(session_ctx_t *ctx);
static bool session_try_command_completion(session_ctx_t *ctx);
static bool session_consume_escape_sequence(session_ctx_t *ctx, char ch);
static session_ctx_t *session_create(void);
static void session_destroy(session_ctx_t *ctx);
static void session_cleanup(session_ctx_t *ctx);
static void *session_thread(void *arg);
static void host_telnet_listener_stop(host_t *host);
static void session_refresh_output_encoding(session_ctx_t *ctx);
static bool session_detect_retro_client(session_ctx_t *ctx);
static void session_telnet_request_terminal_type(session_ctx_t *ctx);
static void session_telnet_capture_startup_metadata(session_ctx_t *ctx);
static void session_history_record(session_ctx_t *ctx, const char *line);
static void session_history_navigate(session_ctx_t *ctx, int direction);
void session_scrollback_reset_position(session_ctx_t *ctx);
void session_scrollback_navigate(session_ctx_t *ctx, int direction,
                                 size_t step);
static bool session_try_localized_command_forward(session_ctx_t *ctx,
                                                  const char *line);
static const char *session_display_name(const session_ctx_t *ctx);
static void chat_history_entry_prepare_user(chat_history_entry_t *entry,
                                            const session_ctx_t *from,
                                            const char *message,
                                            bool preserve_whitespace);
static bool host_history_record_user(host_t *host, const session_ctx_t *from,
                                     const char *message,
                                     bool preserve_whitespace,
                                     chat_history_entry_t *stored_entry);
static bool host_history_commit_entry(host_t *host, chat_history_entry_t *entry,
                                      chat_history_entry_t *stored_entry);
static void host_notify_external_clients(host_t *host,
                                         const chat_history_entry_t *entry);
static bool host_history_append_locked(host_t *host,
                                       const chat_history_entry_t *entry);
static bool host_history_reserve_locked(host_t *host, size_t min_capacity);
static size_t host_history_total(host_t *host);
static size_t host_history_copy_range(host_t *host, size_t start_index,
                                      chat_history_entry_t *buffer,
                                      size_t capacity);
static bool host_history_find_entry_by_id(host_t *host, uint64_t message_id,
                                          chat_history_entry_t *entry);
static size_t host_history_delete_range(host_t *host, uint64_t start_id,
                                        uint64_t end_id,
                                        uint64_t *first_removed,
                                        uint64_t *last_removed,
                                        size_t *replies_removed);
static void chat_room_broadcast_should_sink(chat_room_t *room);
static void chat_room_broadcast_entry(chat_room_t *room,
                                      const chat_history_entry_t *entry,
                                      const session_ctx_t *from);
static void chat_room_broadcast(chat_room_t *room, const char *message,
                                const session_ctx_t *from);
static void chat_room_broadcast_caption(chat_room_t *room, const char *message);
static bool host_history_apply_reaction(host_t *host, uint64_t message_id,
                                        size_t reaction_index,
                                        chat_history_entry_t *updated_entry);
static bool
chat_history_entry_build_reaction_summary(const chat_history_entry_t *entry,
                                          char *buffer, size_t length,
                                          session_ui_language_t lang);
static void host_ban_resolve_path(host_t *host);
static void host_ban_state_save_locked(host_t *host);
static void host_ban_state_load(host_t *host);
static void host_reply_state_resolve_path(host_t *host);
static void host_reply_state_save_locked(host_t *host);
static void host_reply_state_load(host_t *host);
static bool host_replies_find_entry_by_id(host_t *host, uint64_t reply_id,
                                          chat_reply_entry_t *entry);
static bool host_replies_commit_entry(host_t *host, chat_reply_entry_t *entry,
                                      chat_reply_entry_t *stored_entry);
static void session_send_reply_tree(session_ctx_t *ctx,
                                    uint64_t parent_message_id,
                                    uint64_t parent_reply_id, size_t depth);

static const char *session_display_name(const session_ctx_t *ctx)
{
    if (ctx == nullptr) {
        return "";
    }

    if (ctx->user_data_loaded && ctx->user_data.preferred_nickname[0] != '\0') {
        return ctx->user_data.preferred_nickname;
    }

    if (ctx->user.name[0] != '\0') {
        return ctx->user.name;
    }

    return "";
}

// static void host_broadcast_reply(host_t *host, const chat_reply_entry_t *entry);
static void session_send_private_message_line(session_ctx_t *ctx,
                                              const session_ctx_t *color_source,
                                              const char *label,
                                              const char *message);
static session_ctx_t *chat_room_find_user(chat_room_t *room,
                                          const char *username);
static bool host_is_ip_banned(host_t *host, const char *ip);
static bool host_is_username_banned(host_t *host, const char *username);
static bool host_add_ban_entry(host_t *host, const char *username,
                               const char *ip);
static bool host_remove_ban_entry(host_t *host, const char *token);
static join_activity_entry_t *host_find_join_activity_locked(host_t *host,
                                                             const char *ip);
static join_activity_entry_t *host_ensure_join_activity_locked(host_t *host,
                                                               const char *ip);
static bool host_register_suspicious_activity(host_t *host,
                                              const char *username,
                                              const char *ip,
                                              size_t *attempts_out);
static bool session_is_private_ipv4(const unsigned char octets[4]);
bool session_is_lan_client(const char *ip);
static void session_assign_lan_privileges(session_ctx_t *ctx);
static void session_apply_granted_privileges(session_ctx_t *ctx);
static void session_apply_theme_defaults(session_ctx_t *ctx);
static void session_apply_system_theme_defaults(session_ctx_t *ctx);
static void session_force_dark_mode_foreground(session_ctx_t *ctx);
static void session_apply_saved_preferences(session_ctx_t *ctx);
static void session_dispatch_command(session_ctx_t *ctx, const char *line);
static void session_handle_exit(session_ctx_t *ctx);
static void session_force_disconnect(session_ctx_t *ctx, const char *reason);
static void session_handle_nick(session_ctx_t *ctx, const char *arguments);
static bool host_lookup_member_ip(host_t *host, const char *username, char *ip,
                                  size_t length);
static bool host_lookup_last_ip(host_t *host, const char *username, char *ip,
                                size_t length);
static bool session_should_hide_entry(session_ctx_t *ctx,
                                      const chat_history_entry_t *entry);
static bool session_blocklist_add(session_ctx_t *ctx, const char *ip,
                                  const char *username, bool ip_wide,
                                  bool *already_present);
static bool session_blocklist_remove(session_ctx_t *ctx, const char *token);
static void session_blocklist_show(session_ctx_t *ctx);
static void session_handle_reply(session_ctx_t *ctx, const char *arguments);
static void session_handle_block(session_ctx_t *ctx, const char *arguments);
static void session_handle_unblock(session_ctx_t *ctx, const char *arguments);
static void session_handle_pm(session_ctx_t *ctx, const char *arguments);
static void session_handle_motd(session_ctx_t *ctx);
static void session_handle_system_color(session_ctx_t *ctx,
                                        const char *arguments);
static void session_handle_palette(session_ctx_t *ctx, const char *arguments);
static void session_handle_translate(session_ctx_t *ctx, const char *arguments);
static void session_handle_translate_scope(session_ctx_t *ctx,
                                           const char *arguments);
static void session_handle_gemini(session_ctx_t *ctx, const char *arguments);
static void session_handle_captcha(session_ctx_t *ctx, const char *arguments);
static void session_handle_filestore(session_ctx_t *ctx,
                                     const char *arguments);
static void session_handle_filestore_upload(session_ctx_t *ctx,
                                            const char *arguments);
static void session_handle_filestore_download(session_ctx_t *ctx,
                                              const char *arguments);
static void session_handle_set_trans_lang(session_ctx_t *ctx,
                                          const char *arguments);
static void session_handle_set_target_lang(session_ctx_t *ctx,
                                           const char *arguments);
static void session_handle_chat_spacing(session_ctx_t *ctx,
                                        const char *arguments);
static void session_handle_mode(session_ctx_t *ctx, const char *arguments);
static void session_handle_history(session_ctx_t *ctx, const char *arguments);
static void session_handle_eliza(session_ctx_t *ctx, const char *arguments);
static void session_handle_status(session_ctx_t *ctx, const char *arguments);
static void session_handle_showstatus(session_ctx_t *ctx,
                                      const char *arguments);
static void session_handle_weather(session_ctx_t *ctx, const char *arguments);
static void session_handle_pardon(session_ctx_t *ctx, const char *arguments);
static void session_handle_ban_name(session_ctx_t *ctx, const char *arguments);
static void session_handle_ban_list(session_ctx_t *ctx, const char *arguments);
static bool host_add_operator_grant_locked(host_t *host, const char *ip);
static void host_apply_grant_to_ip(host_t *host, const char *ip);
static void host_state_save_locked(host_t *host);
static void session_handle_grant(session_ctx_t *ctx, const char *arguments)
{
    if (ctx == nullptr || ctx->owner == nullptr) {
        return;
    }

    if (!ctx->user.is_operator && !ctx->user.is_lan_operator) {
        session_send_system_line(
            ctx, "Only operators may grant operator privileges.");
        return;
    }

    if (arguments == nullptr) {
        session_send_system_line(ctx, "Usage: /grant <ip-address>");
        return;
    }

    char ip[SSH_CHATTER_IP_LEN];
    snprintf(ip, sizeof(ip), "%s", arguments);
    trim_whitespace_inplace(ip);
    if (ip[0] == '\0') {
        session_send_system_line(ctx, "Usage: /grant <ip-address>");
        return;
    }

    unsigned char buf[sizeof(struct in6_addr)];
    if (inet_pton(AF_INET, ip, buf) != 1 && inet_pton(AF_INET6, ip, buf) != 1) {
        session_send_system_line(ctx, "Provide a valid IPv4 or IPv6 address.");
        return;
    }

    ttak_mutex_lock(&ctx->owner->lock);
    bool added = host_add_operator_grant_locked(ctx->owner, ip);
    if (added) {
        host_state_save_locked(ctx->owner);
    }
    ttak_mutex_unlock(&ctx->owner->lock);

    if (!added) {
        session_send_system_line(ctx, "That IP address already has a grant.");
        return;
    }

    host_apply_grant_to_ip(ctx->owner, ip);

    char message[SSH_CHATTER_MESSAGE_LIMIT];
    snprintf(message, sizeof(message), "Operator privileges granted to %s.",
             ip);
    session_send_system_line(ctx, message);
}

static void session_handle_grant(session_ctx_t *ctx, const char *arguments);
static void session_handle_kick(session_ctx_t *ctx, const char *arguments);
static void session_handle_usercount(session_ctx_t *ctx);
static bool host_username_reserved(host_t *host, const char *username);
static void session_handle_search(session_ctx_t *ctx, const char *arguments);
static void session_handle_chat_lookup(session_ctx_t *ctx,
                                       const char *arguments);
static void session_handle_image(session_ctx_t *ctx, const char *arguments);
static void session_handle_video(session_ctx_t *ctx, const char *arguments);
static void session_handle_audio(session_ctx_t *ctx, const char *arguments);
static void session_handle_files(session_ctx_t *ctx, const char *arguments);
static void session_handle_reaction(session_ctx_t *ctx, size_t reaction_index,
                                    const char *arguments);
static void session_handle_gameopt(session_ctx_t *ctx, const char *arguments);
static void session_handle_othello_command(session_ctx_t *ctx,
                                           const char *arguments);
static void session_handle_mail(session_ctx_t *ctx, const char *arguments);
static void session_handle_today(session_ctx_t *ctx);
static void session_handle_date(session_ctx_t *ctx, const char *arguments);
static void session_handle_os(session_ctx_t *ctx, const char *arguments);
static void session_handle_getos(session_ctx_t *ctx, const char *arguments);
static void session_handle_getaddr(session_ctx_t *ctx, const char *arguments);
static void session_handle_pair(session_ctx_t *ctx);
static void session_handle_connected(session_ctx_t *ctx);
static bool session_parse_birthday(const char *input, char *normalized,
                                   size_t length);
static void session_handle_birthday(session_ctx_t *ctx, const char *arguments);
static void session_handle_setpw(session_ctx_t *ctx, const char *arguments);
static void session_handle_delpw(session_ctx_t *ctx, const char *arguments);
static void session_handle_shell(session_ctx_t *ctx, const char *arguments);
static void session_handle_revoke(session_ctx_t *ctx, const char *arguments);
static void session_handle_delete_message(session_ctx_t *ctx,
                                          const char *arguments);
static void session_normalize_newlines(char *text);
static bool timezone_sanitize_identifier(const char *input, char *output,
                                         size_t length);
static bool timezone_resolve_identifier(const char *input, char *resolved,
                                        size_t length);
static const palette_descriptor_t *palette_find_descriptor(const char *name);
static bool palette_apply_to_session(session_ctx_t *ctx,
                                     const palette_descriptor_t *descriptor);
static void session_translation_flush_ready(session_ctx_t *ctx);
static bool session_translation_queue_caption(session_ctx_t *ctx,
                                              const char *message,
                                              size_t placeholder_lines);
static void session_translation_reserve_placeholders(session_ctx_t *ctx,
                                                     size_t placeholder_lines);
static void session_translation_clear_queue(session_ctx_t *ctx);
static bool session_translation_worker_ensure(session_ctx_t *ctx);
static void session_translation_worker_shutdown(session_ctx_t *ctx);
static void *session_translation_worker(void *arg);

static bool session_translation_queue_private_message(session_ctx_t *ctx,
                                                      session_ctx_t *target,
                                                      const char *message);
static void session_translation_normalize_output(char *text);
static void host_handle_translation_quota_exhausted(host_t *host);
static void
session_handle_translation_quota_exhausted(session_ctx_t *ctx,
                                           const char *error_detail);
static bool session_argument_is_disable(const char *token);
static bool session_argument_is_enable(const char *token);
static void session_language_normalize(const char *input, char *normalized,
                                       size_t length);
static bool session_language_equals(const char *lhs, const char *rhs);

static const char *session_ui_language_code(session_ui_language_t language);
static const char *session_ui_language_name(session_ui_language_t language,
                                            session_ui_language_t locale);
static const session_ui_locale_t *
session_ui_get_locale(const session_ctx_t *ctx);
static const char *session_command_prefix(const session_ctx_t *ctx);
static int session_utf8_display_width(const char *text);
static void session_format_help_line(session_ctx_t *ctx,
                                     const session_help_entry_t *entry,
                                     const char *description, char *buffer,
                                     size_t length);
static bool session_fetch_weather_summary(const char *city,
                                          char *summary, size_t summary_len);
static void session_handle_poll(session_ctx_t *ctx, const char *arguments);
static void session_handle_vote(session_ctx_t *ctx, size_t option_index);
static void session_handle_named_vote(session_ctx_t *ctx, size_t option_index,
                                      const char *label);
static void session_handle_elect_command(session_ctx_t *ctx,
                                         const char *arguments);
static void session_handle_vote_command(session_ctx_t *ctx,
                                        const char *arguments,
                                        bool allow_multiple);
static void session_handle_alpha_centauri_landers(session_ctx_t *ctx);
static void session_format_help_entries_to_buffer(
    session_ctx_t *ctx, const session_help_entry_t *entries, size_t count,
    char *buffer, size_t buffer_length);
static void session_print_help(session_ctx_t *ctx);
static void session_handle_set_ui_lang(session_ctx_t *ctx,
                                       const char *arguments);
static bool session_line_is_exit_command(const char *line);
static void session_handle_username_conflict_input(session_ctx_t *ctx,
                                                   const char *line);
static const char *session_consume_token(const char *input, char *token,
                                         size_t length);
static bool session_user_data_available(session_ctx_t *ctx);
static bool session_user_data_load(session_ctx_t *ctx);
static bool session_user_data_commit(session_ctx_t *ctx);
static bool host_user_data_send_mail(host_t *host, const char *recipient,
                                     const char *recipient_ip,
                                     const char *sender, const char *message,
                                     char *error, size_t error_length);
bool host_user_data_load_existing(host_t *host, const char *username,
                                  const char *ip, user_data_record_t *record,
                                  bool create_if_missing);
static void host_user_data_bootstrap(host_t *host);
static bool session_parse_color_arguments(char *working, char **tokens,
                                          size_t max_tokens,
                                          size_t *token_count);
static size_t session_utf8_prev_char_len(const char *buffer, size_t length);
static int session_utf8_char_width(const char *bytes, size_t length);
static bool host_history_record_system(host_t *host, const char *message,
                                       chat_history_entry_t *stored_entry);
static void host_history_cleanup_expired(host_t *host);
static void session_send_history_entry(session_ctx_t *ctx,
                                       const chat_history_entry_t *entry);
static void session_deliver_outgoing_message(session_ctx_t *ctx,
                                             const char *message,
                                             bool clear_prompt_text);
static void
chat_room_broadcast_reaction_update(host_t *host,
                                    const chat_history_entry_t *entry);
static user_preference_t *
host_find_preference_locked(host_t *host, const char *username, const char *ip);
static user_preference_t *host_ensure_preference_locked(host_t *host,
                                                        const char *username,
                                                        const char *ip);
static void host_store_user_theme(host_t *host, session_ctx_t *ctx);
static size_t host_prepare_join_delay(host_t *host,
                                      struct timespec *wait_duration);
static host_join_attempt_result_t
host_register_join_attempt(host_t *host, const char *username, const char *ip);
static bool session_run_captcha(session_ctx_t *ctx);
static bool session_is_captcha_exempt(const session_ctx_t *ctx);
static void host_store_system_theme(host_t *host, const session_ctx_t *ctx);
static void host_store_user_os(host_t *host, const session_ctx_t *ctx);
static void host_store_birthday(host_t *host, const session_ctx_t *ctx,
                                const char *birthday);
static void host_store_chat_spacing(host_t *host, const session_ctx_t *ctx);
static void host_store_translation_preferences(host_t *host,
                                               const session_ctx_t *ctx);

static bool host_ip_has_grant_locked(host_t *host, const char *ip);
static bool host_ip_has_grant(host_t *host, const char *ip);
static bool host_remove_operator_grant_locked(host_t *host, const char *ip);
static void host_refresh_motd_locked(host_t *host);
static void host_refresh_motd(host_t *host);
static void host_build_birthday_notice_locked(host_t *host, char *line,
                                              size_t length);
static bool host_is_leap_year(int year);
static void host_revoke_grant_from_ip(host_t *host, const char *ip);
static bool host_history_normalize_entry(host_t *host,
                                         chat_history_entry_t *entry);
static const char *chat_attachment_type_label(chat_attachment_type_t type);
static void host_state_resolve_path(host_t *host);
static void host_state_load(host_t *host);
static void host_state_save_locked(host_t *host);
static void host_ui_language_state_resolve_path(host_t *host);
static void host_ui_language_state_load(host_t *host);
static void host_ui_language_state_save_locked(host_t *host);
static void host_eliza_state_resolve_path(host_t *host);
static void host_eliza_state_load(host_t *host);
static void host_eliza_state_save_locked(host_t *host);
static void host_eliza_memory_resolve_path(host_t *host);
static void host_eliza_memory_load(host_t *host);
static void host_eliza_memory_save_locked(host_t *host);
static void host_eliza_memory_store(host_t *host, const char *prompt,
                                    const char *reply);
static size_t host_eliza_memory_collect_context(host_t *host,
                                                const char *prompt,
                                                char *context,
                                                size_t context_length);
static void host_eliza_history_normalize_line(char *text);
static size_t host_eliza_history_collect_context(host_t *host, char *context,
                                                 size_t context_length);
static void host_eliza_prepare_preview(const char *source, char *dest,
                                       size_t dest_length);
static size_t host_eliza_bbs_collect_context(host_t *host, char *context,
                                             size_t context_length);
static size_t host_eliza_memory_collect_tokens(const char *prompt,
                                               char tokens[][32],
                                               size_t max_tokens);
static void host_bbs_resolve_path(host_t *host);
static void host_bbs_state_load(host_t *host);
static void host_bbs_state_save_locked(host_t *host);
static void host_bbs_start_watchdog(host_t *host);
static void *host_bbs_watchdog_thread(void *arg);
static void host_bbs_watchdog_scan(host_t *host);
static void host_security_configure(host_t *host);
static bool host_ensure_private_data_path(host_t *host, const char *path,
                                          bool create_directories);
static void host_security_disable_filter(host_t *host, const char *reason);
static host_security_scan_result_t
host_security_scan_payload(host_t *host, const char *category,
                           const char *payload, size_t length, char *diagnostic,
                           size_t diagnostic_length);
static void host_security_process_blocked(host_t *host, const char *category,
                                          const char *diagnostic,
                                          const char *username, const char *ip,
                                          session_ctx_t *session,
                                          bool post_send, const char *content);
static void host_security_process_error(host_t *host, const char *category,
                                        const char *diagnostic,
                                        const char *username, const char *ip,
                                        session_ctx_t *session, bool post_send);
static bool host_moderation_init(host_t *host);
static void host_moderation_shutdown(host_t *host);
static void host_moderation_backoff(unsigned int attempts);
static bool host_moderation_spawn_worker(host_t *host);
static void host_moderation_close_worker(host_t *host);
static bool host_moderation_recover_worker(host_t *host,
                                           const char *diagnostic);
static bool host_moderation_queue_chat(session_ctx_t *ctx, const char *message,
                                       size_t length);
static void *host_moderation_thread(void *arg);
static bool host_moderation_write_all(int fd, const void *buffer,
                                      size_t length);
static bool host_moderation_read_all(int fd, void *buffer, size_t length);
static void host_moderation_worker_loop(int request_fd, int response_fd);
static void host_moderation_handle_failure(host_t *host,
                                           host_moderation_task_t *task,
                                           const char *diagnostic);
static void
host_moderation_apply_result(host_t *host, host_moderation_task_t *task,
                             const host_moderation_ipc_response_t *response,
                             const char *message);
static void host_moderation_flush_pending(host_t *host, const char *diagnostic);
static double host_elapsed_seconds(const struct timespec *start,
                                   const struct timespec *end);
static bool host_eliza_enable(host_t *host);
static bool host_eliza_disable(host_t *host);
static void host_eliza_announce_join(host_t *host);
static void host_eliza_announce_depart(host_t *host);
static void host_eliza_say(host_t *host, const char *message);
static void host_eliza_prepare_private_reply(const char *message, char *reply,
                                             size_t reply_length);
static bool host_eliza_content_is_severe(const char *text);
static bool host_eliza_worker_init(host_t *host);
static void host_eliza_worker_shutdown(host_t *host);
static bool host_eliza_worker_enqueue(host_t *host,
                                      host_eliza_intervene_task_t *task);
static void *host_eliza_worker_thread(void *arg);
static bool host_eliza_intervene(session_ctx_t *ctx, const char *content,
                                 const char *reason, bool from_filter);
static void host_eliza_intervene_execute(session_ctx_t *ctx, const char *reason,
                                         bool from_filter);
static host_security_scan_result_t
session_security_check_text(session_ctx_t *ctx, const char *category,
                            const char *content, size_t length, bool post_send);
static void host_vote_resolve_path(host_t *host);
static void host_vote_state_load(host_t *host);
static void host_vote_state_save_locked(host_t *host);
static bool host_try_load_motd_from_path(host_t *host, const char *path);
static bool username_contains(const char *username, const char *needle);
static void
host_apply_palette_descriptor(host_t *host,
                              const palette_descriptor_t *descriptor);
static bool host_lookup_user_os(host_t *host, const char *username,
                                char *buffer, size_t length);
static void session_send_poll_summary(session_ctx_t *ctx);
static void session_send_poll_summary_generic(session_ctx_t *ctx,
                                              const poll_state_t *poll,
                                              const char *label);
static void session_list_named_polls(session_ctx_t *ctx);
static void session_handle_bbs(session_ctx_t *ctx, const char *arguments);
static void poll_state_reset(poll_state_t *poll);
static void named_poll_reset(named_poll_state_t *poll);
static named_poll_state_t *host_find_named_poll_locked(host_t *host,
                                                       const char *label);
static named_poll_state_t *host_ensure_named_poll_locked(host_t *host,
                                                         const char *label);
static void host_recount_named_polls_locked(host_t *host);
static bool poll_label_is_valid(const char *label);
static void session_bbs_show_dashboard(session_ctx_t *ctx);
static void session_bbs_list(session_ctx_t *ctx, const char *arguments);
static void session_bbs_select_board(session_ctx_t *ctx, const char *arguments);
static void session_bbs_list_topic(session_ctx_t *ctx, const char *topic);
static void session_bbs_read(session_ctx_t *ctx, uint64_t id);
static void session_bbs_begin_post(session_ctx_t *ctx, const char *arguments);
static void session_bbs_begin_edit(session_ctx_t *ctx, uint64_t id);
static void session_bbs_capture_body_text(session_ctx_t *ctx, const char *text);
static void session_bbs_capture_body_line(session_ctx_t *ctx, const char *line);
static bool session_bbs_capture_continue(const session_ctx_t *ctx);
static void session_bbs_add_comment(session_ctx_t *ctx, const char *arguments);
static void session_bbs_regen_post(session_ctx_t *ctx, uint64_t id);
static void session_bbs_delete(session_ctx_t *ctx, uint64_t id);
static void session_bbs_reset_pending_post(session_ctx_t *ctx);
static bbs_post_t *host_find_bbs_post_locked(host_t *host, uint64_t id);
static bbs_post_t *host_allocate_bbs_post_locked(host_t *host);
static void host_clear_bbs_post_locked(host_t *host, bbs_post_t *post);

bool host_user_data_load_existing(host_t *host, const char *username,
                                  const char *ip, user_data_record_t *record,
                                  bool create_if_missing);

static bool session_bbs_scroll(session_ctx_t *ctx, int direction, size_t step);
static bool session_bbs_refresh_view(session_ctx_t *ctx);
static void session_handle_rss(session_ctx_t *ctx, const char *arguments);
static void session_rss_list(session_ctx_t *ctx);
static void session_rss_read(session_ctx_t *ctx, const char *tag);
static void session_rss_begin(session_ctx_t *ctx, const char *tag,
                              const rss_session_item_t *items, size_t count);
static void session_rss_show_current(session_ctx_t *ctx);
static bool session_rss_move(session_ctx_t *ctx, int delta);
static void session_rss_exit(session_ctx_t *ctx, const char *reason);
static void session_rss_clear(session_ctx_t *ctx);
static bool session_parse_command(const char *line, const char *command,
                                  const char **arguments);
static bool
session_parse_localized_command(session_ctx_t *ctx,
                                const session_command_alias_t *alias,
                                const char *line, const char **arguments);
static void rss_strip_html(char *text);
static void rss_decode_entities(char *text);
static void rss_trim_whitespace(char *text);
static bool host_asciiart_cooldown_active(host_t *host, const char *ip,
                                          const struct timespec *now,
                                          long *remaining_seconds);
static void host_asciiart_register_post(host_t *host, const char *ip,
                                        const struct timespec *when);
static bool session_asciiart_cooldown_active(session_ctx_t *ctx,
                                             struct timespec *now,
                                             long *remaining_seconds);
static void session_asciiart_reset(session_ctx_t *ctx);
static void session_asciiart_begin(session_ctx_t *ctx,
                                   session_asciiart_target_t target);
static void session_asciiart_import_from_editor(session_ctx_t *ctx);
static void session_asciiart_capture_text(session_ctx_t *ctx, const char *text);
static void session_asciiart_capture_line(session_ctx_t *ctx, const char *line);
static void session_asciiart_commit(session_ctx_t *ctx);
static void session_asciiart_cancel(session_ctx_t *ctx, const char *reason);
typedef void (*session_text_line_consumer_t)(session_ctx_t *, const char *);
typedef bool (*session_text_continue_predicate_t)(const session_ctx_t *);
static void session_capture_multiline_text(
    session_ctx_t *ctx, const char *text, session_text_line_consumer_t consumer,
    session_text_continue_predicate_t should_continue);
static bool session_asciiart_capture_continue(const session_ctx_t *ctx);
static void session_handle_game(session_ctx_t *ctx, const char *arguments);
static void session_game_suspend(session_ctx_t *ctx, const char *reason);
static int session_channel_read_poll(session_ctx_t *ctx, char *buffer,
                                     size_t length, int timeout_ms);
static void session_game_seed_rng(session_ctx_t *ctx);
static uint32_t session_game_random(session_ctx_t *ctx);
static int session_game_random_range(session_ctx_t *ctx, int max);
static void session_game_start_tetris(session_ctx_t *ctx);
static void session_game_tetris_reset(tetris_game_state_t *state);
static void
session_game_tetris_apply_round_settings(tetris_game_state_t *state);
static void session_game_tetris_handle_round_progress(session_ctx_t *ctx);
static void session_game_tetris_fill_bag(session_ctx_t *ctx);
static int session_game_tetris_take_piece(session_ctx_t *ctx);
static bool session_game_tetris_spawn_piece(session_ctx_t *ctx);
static bool session_game_tetris_cell_occupied(int piece, int rotation, int row,
                                              int column);
static bool session_game_tetris_position_valid(const tetris_game_state_t *state,
                                               int piece, int rotation, int row,
                                               int column);
static bool session_game_tetris_move(session_ctx_t *ctx, int drow, int dcol);
static bool session_game_tetris_soft_drop(session_ctx_t *ctx);
static bool session_game_tetris_rotate(session_ctx_t *ctx);
static bool session_game_tetris_apply_gravity(session_ctx_t *ctx,
                                              unsigned ticks);
static bool session_game_tetris_update_timer(session_ctx_t *ctx,
                                             bool accelerate);
static bool session_game_tetris_process_timeout(session_ctx_t *ctx);
static bool session_game_tetris_process_action(session_ctx_t *ctx, int action);
static bool session_game_tetris_process_raw_input(session_ctx_t *ctx, char ch);
static void session_game_tetris_lock_piece(session_ctx_t *ctx);
static void session_game_tetris_clear_lines(session_ctx_t *ctx,
                                            unsigned *cleared);
static void session_game_tetris_render(session_ctx_t *ctx);
static void session_game_tetris_handle_line(session_ctx_t *ctx,
                                            const char *line);
static void session_game_start_liargame(session_ctx_t *ctx);
static void session_game_liar_present_round(session_ctx_t *ctx);
static void session_game_liar_handle_line(session_ctx_t *ctx, const char *line);
static void session_game_toggle_camouflage(session_ctx_t *ctx);
static void session_game_gonu_render(session_ctx_t *ctx);
static bool session_game_gonu_handle_input(session_ctx_t *ctx,
                                           const char *input);
static void session_game_start_alpha(session_ctx_t *ctx);
static void session_game_alpha_reset(session_ctx_t *ctx);
static void session_game_alpha_prepare_navigation(session_ctx_t *ctx);
static void session_game_alpha_reroll_navigation(session_ctx_t *ctx);
static void
session_game_alpha_add_gravity_source(alpha_centauri_game_state_t *state, int x,
                                      int y, double mu, int influence_radius,
                                      char symbol, const char *name);
static void session_game_start_othello(session_ctx_t *ctx);
static void session_game_othello_handle_line(session_ctx_t *ctx,
                                             const char *line);
static void session_game_othello_prepare_next_turn(session_ctx_t *ctx);
static void session_game_othello_finish(session_ctx_t *ctx, const char *reason);
static void session_game_othello_render(session_ctx_t *ctx);
static void session_game_othello_count_scores(const othello_game_state_t *state,
                                              unsigned *red, unsigned *green);
static void session_game_othello_reset_state(othello_game_state_t *state);
static void session_game_othello_handle_ai_turn(session_ctx_t *ctx);
static void session_game_alpha_configure_gravity(session_ctx_t *ctx);
static void
session_game_alpha_apply_gravity(alpha_centauri_game_state_t *state);
static const char *const kAlphaStarCatalog[] = {
    "Midway Star",   "Binary Torch", "Turnover Sun",
    "Arrival Flare", "Relay Star",   "Shepherd Star",
};

static const char *const kAlphaPlanetCatalog[] = {
    "Departure World", "Drift Planet", "Relay Outpost",
    "Approach World",  "Proxima b",    "Immigrants' Harbor",
};

static const char *const kAlphaDebrisCatalog[] = {
    "Comet Trail", "Asteroid Swarm", "Ice Shard", "Dust Ribbon", "Sail Wreck",
};

#define ALPHA_STAR_CATALOG_COUNT                                               \
    (sizeof(kAlphaStarCatalog) / sizeof(kAlphaStarCatalog[0]))
#define ALPHA_PLANET_CATALOG_COUNT                                             \
    (sizeof(kAlphaPlanetCatalog) / sizeof(kAlphaPlanetCatalog[0]))
#define ALPHA_DEBRIS_CATALOG_COUNT                                             \
    (sizeof(kAlphaDebrisCatalog) / sizeof(kAlphaDebrisCatalog[0]))

static bool
session_game_alpha_position_occupied(const alpha_centauri_game_state_t *state,
                                     int x, int y)
{
    if (state == nullptr) {
        return true;
    }
    if (state->nav_x == x && state->nav_y == y) {
        return true;
    }
    if (state->nav_target_x == x && state->nav_target_y == y) {
        return true;
    }
    for (unsigned idx = 0U; idx < state->gravity_source_count; ++idx) {
        const alpha_gravity_source_t *existing = &state->gravity_sources[idx];
        if (existing->x == x && existing->y == y) {
            return true;
        }
    }
    if (state->stage == 4U) {
        if (!state->eva_ready) {
            for (unsigned idx = 0U; idx < state->waypoint_count; ++idx) {
                const alpha_waypoint_t *waypoint = &state->waypoints[idx];
                if (waypoint->x == x && waypoint->y == y) {
                    return true;
                }
            }
        }
        if (state->final_waypoint.symbol != '\0' &&
            state->final_waypoint.x == x && state->final_waypoint.y == y) {
            return true;
        }
    }
    return false;
}

static void session_game_alpha_place_random_source(
    session_ctx_t *ctx, alpha_centauri_game_state_t *state, int margin,
    double mu, int radius, char symbol, const char *name)
{
    if (ctx == nullptr || state == nullptr) {
        return;
    }

    int attempts = 0;
    int min_margin = margin >= 0 ? margin : 0;
    int usable_width = ALPHA_NAV_WIDTH - (min_margin * 2);
    int usable_height = ALPHA_NAV_HEIGHT - (min_margin * 2);
    if (usable_width <= 0) {
        usable_width = ALPHA_NAV_WIDTH;
        min_margin = 0;
    }
    if (usable_height <= 0) {
        usable_height = ALPHA_NAV_HEIGHT;
        min_margin = 0;
    }

    while (attempts < 128) {
        int x = min_margin + session_game_random_range(ctx, usable_width);
        int y = min_margin + session_game_random_range(ctx, usable_height);
        if (!session_game_alpha_position_occupied(state, x, y)) {
            session_game_alpha_add_gravity_source(state, x, y, mu, radius,
                                                  symbol, name);
            return;
        }
        ++attempts;
    }

    int fallback_x = min_margin < ALPHA_NAV_WIDTH ? min_margin : 0;
    int fallback_y = min_margin < ALPHA_NAV_HEIGHT ? min_margin : 0;
    session_game_alpha_add_gravity_source(state, fallback_x, fallback_y, mu,
                                          radius, symbol, name);
}

static double session_game_alpha_random_double(session_ctx_t *ctx,
                                               double min_value,
                                               double max_value)
{
    if (ctx == nullptr) {
        return min_value;
    }
    if (max_value <= min_value) {
        return min_value;
    }
    double fraction = (double)session_game_random(ctx) / (double)UINT32_MAX;
    if (fraction < 0.0) {
        fraction = 0.0;
    } else if (fraction > 1.0) {
        fraction = 1.0;
    }
    return min_value + (max_value - min_value) * fraction;
}

static int session_game_alpha_random_with_margin(session_ctx_t *ctx, int extent,
                                                 int margin)
{
    if (extent <= 0) {
        return 0;
    }
    int safe_margin = margin;
    if (safe_margin < 0) {
        safe_margin = 0;
    }
    int usable = extent - (safe_margin * 2);
    if (usable <= 0) {
        usable = extent;
        safe_margin = 0;
    }
    return safe_margin + session_game_random_range(ctx, usable);
}
static void session_game_alpha_sync_from_save(session_ctx_t *ctx);
static void session_game_alpha_sync_to_save(session_ctx_t *ctx);
static void session_game_alpha_present_stage(session_ctx_t *ctx);
static void session_game_alpha_handle_line(session_ctx_t *ctx,
                                           const char *line);
static void session_game_alpha_log_completion(session_ctx_t *ctx);
static void session_game_alpha_render_navigation(session_ctx_t *ctx);
static void session_game_alpha_refresh_navigation(session_ctx_t *ctx);
static void session_game_alpha_plan_waypoints(session_ctx_t *ctx);
static void session_game_alpha_present_waypoints(session_ctx_t *ctx);
static void session_game_alpha_complete_waypoint(session_ctx_t *ctx);
static bool session_game_alpha_handle_arrow(session_ctx_t *ctx, int dx, int dy);
static bool session_game_alpha_attempt_completion(session_ctx_t *ctx);
static void session_game_alpha_execute_ignite(session_ctx_t *ctx);
static void session_game_alpha_execute_trim(session_ctx_t *ctx);
static void session_game_alpha_execute_flip(session_ctx_t *ctx);
static void session_game_alpha_execute_retro(session_ctx_t *ctx);
static void session_game_alpha_execute_eva(session_ctx_t *ctx);
static void session_game_alpha_manual_lock(session_ctx_t *ctx);
static void session_game_alpha_manual_save(session_ctx_t *ctx);
static void host_update_last_captcha_prompt(host_t *host,
                                            const captcha_prompt_t *prompt,
                                            const captcha_language_t *order,
                                            size_t count);

typedef struct liar_prompt {
    const char *statements[3];
    unsigned liar_index;
} liar_prompt_t;

static const liar_prompt_t LIAR_PROMPTS[] = {
    {{"I have contributed code to an open source project.",
      "I once replaced an entire server rack solo.",
      "I prefer mechanical keyboards with clicky switches."},
     1U},
    {{"I have memorized pi to 200 digits.",
      "I used to write BASIC games in middle school.",
      "I cannot solve a Rubik's Cube."},
     0U},
    {{"I drink my coffee without sugar.",
      "I debug using `printf` more than any other tool.",
      "I have never broken a build."},
     2U},
    {{"I run Linux on my primary laptop.",
      "I have camped overnight for a console launch.",
      "I have attended a demoparty."},
     1U},
    {{"I know how to solder surface-mount components.",
      "I have written an emulator in C.", "I have a pet snake named Segfault."},
     2U},
    {{"I play at least one rhythm game competitively.",
      "I once deployed to production from my phone.",
      "I have built a keyboard from scratch."},
     1U},
};

static const char *const TETROMINO_SHAPES[7][4] = {
    {
        "...."
        "####"
        "...."
        "....",
        "..#."
        "..#."
        "..#."
        "..#.",
        "...."
        "####"
        "...."
        "....",
        "..#."
        "..#."
        "..#."
        "..#.",
    },
    {
        "#..."
        "###."
        "...."
        "....",
        ".##."
        ".#.."
        ".#.."
        "....",
        "...."
        "###."
        "..#."
        "....",
        ".#.."
        ".#.."
        "##.."
        "....",
    },
    {
        "..#."
        "###."
        "...."
        "....",
        ".#.."
        ".#.."
        ".##."
        "....",
        "...."
        "###."
        "#..."
        "....",
        "##.."
        ".#.."
        ".#.."
        "....",
    },
    {
        ".##."
        ".##."
        "...."
        "....",
        ".##."
        ".##."
        "...."
        "....",
        ".##."
        ".##."
        "...."
        "....",
        ".##."
        ".##."
        "...."
        "....",
    },
    {
        ".##."
        "##.."
        "...."
        "....",
        ".#.."
        ".##."
        "..#."
        "....",
        ".##."
        "##.."
        "...."
        "....",
        ".#.."
        ".##."
        "..#."
        "....",
    },
    {
        ".#.."
        "###."
        "...."
        "....",
        ".#.."
        ".##."
        ".#.."
        "....",
        "...."
        "###."
        ".#.."
        "....",
        ".#.."
        "##.."
        ".#.."
        "....",
    },
    {
        "##.."
        ".##."
        "...."
        "....",
        "..#."
        ".##."
        ".#.."
        "....",
        "##.."
        ".##."
        "...."
        "....",
        "..#."
        ".##."
        ".#.."
        "....",
    },
};

static const char TETROMINO_DISPLAY_CHARS[7] = {'I', 'J', 'L', 'O',
                                                'S', 'T', 'Z'};
