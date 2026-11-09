/*
    Stremio Switch — borealis front-end (official Horizon-style UI).
*/

#include <borealis.hpp>

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <sys/stat.h>
#include <memory>
#include <vector>

#include <borealis/views/applet_frame.hpp>
#include <borealis/views/button.hpp>

#include <curl/curl.h>

#include <switch.h>  // Thread/Mutex/CondVar for the async log sink

#include "appdata.hpp"
#include "browse.hpp"
#include "config.hpp"
#include "settings.hpp"
#include "theme.hpp"
#include "update.hpp"
#include "stremio.hpp"

extern "C" {
#include "torrentfs.h"
}

#include "i18n.hpp"

namespace
{

// --- Async log sink ---------------------------------------------------------
// The log used to be an UNBUFFERED FILE handed to the Logger: every line
// (each mpv "v" message, the 2 s [stats] line) was a synchronous fs IPC from
// whichever thread logged it -- including the render thread, from draw(). One
// SD-card internal-GC stall blocked rendering for as long as the card pleased
// (measured: a 941 ms draw(), audio underrun included, with every engine
// probe healthy). Logger lines now land in a RAM ring through funopen(); a
// dedicated low-priority thread drains the ring to the SD file and flushes.
// The drain wakes on every line, so a crash still loses at most the few
// milliseconds of lines the stall itself would have eaten.
constexpr size_t kLogRingCap = 256 * 1024;

struct LogRing
{
    FILE* out = nullptr;   // the real SD file
    Mutex lock;
    CondVar cv;
    Thread thread;
    volatile bool stop = false;
    size_t head = 0, len = 0;
    bool overflowed = false;
    char buf[kLogRingCap];
};
LogRing* g_logRing = nullptr;

void logDrainThread(void*)
{
    LogRing* r = g_logRing;
    static char tmp[8 * 1024];  // one drainer; static keeps the stack tiny
    for (;;)
    {
        mutexLock(&r->lock);
        while (r->len == 0 && !r->stop)
            condvarWaitTimeout(&r->cv, &r->lock, 1000000000ULL);  // 1 s
        if (r->len == 0 && r->stop)
        {
            mutexUnlock(&r->lock);
            break;
        }
        size_t n = r->len < sizeof(tmp) ? r->len : sizeof(tmp);
        size_t first = kLogRingCap - r->head;
        if (first > n) first = n;
        memcpy(tmp, r->buf + r->head, first);
        memcpy(tmp + first, r->buf, n - first);
        r->head = (r->head + n) % kLogRingCap;
        r->len -= n;
        bool note = r->overflowed;
        r->overflowed = false;
        mutexUnlock(&r->lock);

        // SD I/O happens here, outside the ring lock: a stalling card blocks
        // only this thread, never a logger.
        std::fwrite(tmp, 1, n, r->out);
        if (note)
        {
            static const char msg[] = "[log] ring overflow: lines dropped\n";
            std::fwrite(msg, 1, sizeof(msg) - 1, r->out);
        }
        std::fflush(r->out);
    }
    std::fflush(r->out);
}

// funopen() write callback: memcpy into the ring and return. Never touches
// the fs. On overflow (SD slower than the log rate for a long stretch) lines
// are dropped and the drain thread notes it in the file.
int logRingWrite(void* cookie, const char* p, size_t n)
{
    LogRing* r = static_cast<LogRing*>(cookie);
    if (n == 0) return 0;
    mutexLock(&r->lock);
    size_t space = kLogRingCap - r->len;
    size_t take = n <= space ? n : space;
    if (take < n) r->overflowed = true;
    size_t tail = (r->head + r->len) % kLogRingCap;
    size_t first = kLogRingCap - tail;
    if (first > take) first = take;
    memcpy(r->buf + tail, p, first);
    memcpy(r->buf, p + first, take - first);
    r->len += take;
    mutexUnlock(&r->lock);
    condvarWakeOne(&r->cv);
    return (int)n;  // claim everything: a short write would make stdio error out
}

// Everything the app writes (log, poster and subtitle caches) lives here so it
// doesn't clutter the SD card root. Created at startup. Paths in appdata.hpp.
void ensureAppDataDir()
{
    mkdir("sdmc:/switch", 0777);  // usually already there
    mkdir(APPDATA_DIR, 0777);     // ignore EEXIST
    mkdir(APPDATA_POSTERS, 0777); // cached Stremio artwork
    mkdir(APPDATA_SUBS, 0777);    // subtitles pulled from Stremio addons
}

// Browser frame background for the Stremio tab: diagonal gradient from
// top-right (#1a173e) to bottom-left (#000000), painted behind header/content/footer.
class BrowserFrame : public brls::AppletFrame
{
  public:
    // On teardown -- app exit above all -- the header views this frame owns and
    // the live StremioTab beneath it are freed in an order we do not control.
    // Several global sinks bridge those two subtrees: viewTabSink captures the
    // header's view bar and its buttons, libraryUpTarget a header button, the
    // cyclers/selector the tab. As it is destroyed ~StremioTab calls
    // reportView(-1), which routes through viewTabSink into what may already be a
    // freed header view -- the crash on quitting from the signed-in Stremio tab.
    // A subclass destructor body runs BEFORE the base Box destroys our children,
    // so clearing every bridge here makes those late calls harmless no-ops.
    ~BrowserFrame() override
    {
        // Same reasoning for the repaint hook: it captures the header's two bars.
        theme::setRepaintHook(nullptr);
        stremio::setViewTabSink(nullptr);
        stremio::setViewSelector(nullptr);
        stremio::setViewCycler(nullptr);
        stremio::setLibraryUpTarget(nullptr);
        stremio::setLibraryCountSink(nullptr);
    }

    // Both the background gradient and the header mark are Stremio-tab-only.
    void setOnStremio(bool on) { this->onStremio = on; }

    void draw(NVGcontext* vg, float x, float y, float width, float height,
              brls::Style style, brls::FrameContext* ctx) override
    {
        if (this->onStremio)
        {
            NVGpaint gradient =
                nvgLinearGradient(vg, x + width, y, x, y + height,
                                  theme::gradTop(), theme::gradBottom());
            nvgBeginPath(vg);
            nvgRect(vg, x, y, width, height);
            nvgFillPaint(vg, gradient);
            nvgFill(vg);
        }
        brls::AppletFrame::draw(vg, x, y, width, height, style, ctx);

        // The Stremio mark, drawn on top of the header's icon slot rather than
        // loaded as a texture, so it follows the accent colour. applyTabIdentity
        // leaves that Image INVISIBLE (not GONE) on this tab: it still reserves
        // the same box, which is where we paint. AFTER the base draw, since the
        // header is one of its children -- and safe to batch normally, unlike the
        // player, because nothing here renders outside nanovg.
        if (this->onStremio)
        {
            if (!this->iconSlot)
                this->iconSlot = this->getView("brls/applet_frame/title_icon");
            if (this->iconSlot)
                theme::drawStremioMark(vg, this->iconSlot->getX(),
                                       this->iconSlot->getY(),
                                       this->iconSlot->getWidth());
        }
    }

  private:
    bool onStremio = false;
    // Resolved once: getView() walks the tree by id, which has no business
    // running every frame.
    brls::View* iconSlot = nullptr;
};

// Header identity. The Stremio mark is drawn by BrowserFrame, so no texture is
// loaded for the header icon at all.
void applyTabIdentity(brls::AppletFrame* frame)
{
    if (auto* browserFrame = dynamic_cast<BrowserFrame*>(frame))
        browserFrame->setOnStremio(true);

    frame->setTitle("");

    if (auto* ic = dynamic_cast<brls::Image*>(
            frame->getView("brls/applet_frame/title_icon")))
    {
        ic->setDimensions(54.0f, 54.0f);
        ic->setScalingType(brls::ImageScalingType::FIT);
        // INVISIBLE, not GONE: it must still reserve its slot in the header row,
        // because that box is exactly where the mark gets painted.
        ic->setVisibility(brls::Visibility::INVISIBLE);
    }
}

// A Label that renders with the Material icon font as its PRIMARY face. Reached
// through the regular font's fallback, a Material glyph is positioned on the
// text baseline and sits visibly too high; as the primary face, nanovg centres
// it with the icon font's own metrics, level with the text beside it.
class IconLabel : public brls::Label
{
  public:
    IconLabel()
    {
        int f = brls::Application::getFont(brls::FONT_MATERIAL_ICONS);
        if (f >= 0) this->font = f;
    }
};

// The Stremio header: the sub-view switcher centred, Search / account /
// Options top-right.
void attachTopTabBar(brls::AppletFrame* frame, brls::Box* content)
{
    // The accent drives the focused/active buttons (BUTTONSTYLE_PRIMARY reads
    // brls/accent) and the focus highlight; main() has already applied it, but
    // the header is also rebuilt on a colour change, and re-applying costs
    // nothing.
    theme::applyAccent();

    // The app has a single tab now. It is added here rather than by a tab bar,
    // and lives as long as the frame.
    auto* tab = new StremioTab();
    tab->setGrow(1.0f);
    content->addView(tab);
    applyTabIdentity(frame);

    // The Stremio sub-view switcher, centred in the header: one button per view
    // (Continue / Movies / Shows / Library). It says where you are and clicking
    // one jumps straight there, alongside the L/R cycle. The live StremioTab
    // drives it -- setViewTabSink highlights the active view, or hides the whole
    // bar (index -1) on the sign-in screen.
    auto* viewBar = new brls::Box();
    viewBar->setAxis(brls::Axis::ROW);
    viewBar->setAlignItems(brls::AlignItems::CENTER);
    viewBar->setJustifyContent(brls::JustifyContent::CENTER);

    // The right-hand cluster: Search (the fifth view, pulled out of the group
    // and kept as its own icon), the account button and Options.
    auto* rightBar = new brls::Box();
    rightBar->setAxis(brls::Axis::ROW);
    rightBar->setAlignItems(brls::AlignItems::CENTER);

    std::vector<brls::Button*> viewBtns;
    std::vector<brls::Label*>  viewIcons;
    const float btnFont    = brls::Application::getStyle()["brls/button/text_size"];

    // A Label measured by its own font makes its button a different size: the
    // vertical bounds nanovg reports are the FONT's line height, and Material
    // Icons' is far taller than the text face's, so an icon button came out
    // taller than the text buttons beside it -- and wider, since the same
    // measure includes the glyph's side bearings. Pinning the icon to a box of
    // its own settles both: a height under the text label's leaves the button
    // sized by the text exactly as a plain one, and an exact width drops the
    // bearings. The glyph is then drawn centred in that box, whatever its size
    // -- which is what lets the bare icons below be bigger than the text.
    // withText: the glyph has to sit level with a text label beside it, which is
    // not the same thing as sitting in the middle of the button. "Middle"
    // anchors the ink halfway between the font's OWN ascender and descender, and
    // the text face puts its ink higher above that point than the icon face
    // does -- so an icon centred like its neighbour reads as low. Opposite
    // margins lift it by that difference without changing the box's outer height
    // (and so the button's). A bare icon has nothing to line up with and simply
    // stays centred, where its cursor and background are.
    auto sizeIcon = [btnFont](brls::Label* ic, float size, bool withText) {
        ic->setFontSize(size);
        ic->setWidth(size);
        ic->setHeight(btnFont);
        ic->setHorizontalAlign(brls::HorizontalAlign::CENTER);
        ic->setVerticalAlign(brls::VerticalAlign::CENTER);
        float nudge = withText ? size / 9.0f : 0.0f;
        ic->setMargins(-nudge, 0.0f, nudge, 0.0f);
    };

    // Standing alone, an icon reads as small at the buttons' text size -- so the
    // ones with no label beside them (Search, account, Options) are drawn half
    // again as big. Only the glyph grows: their box is the same height, so the
    // buttons stay level with the rest of the header.
    const float bareIcon = btnFont * 1.5f;

    const auto& viewLabels = stremio::viewLabels();
    // Search is last in the view cycle (and so in the labels, which are
    // index-matched to it). It is the one view shown as a bare icon.
    const size_t searchIdx = viewLabels.size() - 1;
    for (size_t i = 0; i < viewLabels.size(); i++)
    {
        // Each label is "<material-glyph> <text>". Rendered as one Button string
        // the glyph shares the text's baseline and sits visibly too high. Split
        // it out into its own Label: the Button is a row with alignItems=center,
        // so a separate icon Label is centred against the text instead.
        const std::string& full = viewLabels[i];
        size_t      sp    = full.find(' ');
        std::string glyph = sp == std::string::npos ? std::string() : full.substr(0, sp);
        std::string text  = sp == std::string::npos ? full : full.substr(sp + 1);
        bool iconOnly     = i == searchIdx;

        auto* b = new brls::Button();
        if (!iconOnly) b->setText(text);
        b->setStyle(&brls::BUTTONSTYLE_BORDERLESS);
        if (i && !iconOnly) b->setMarginLeft(2.0f);

        auto* ic = new IconLabel();
        ic->setText(glyph);
        sizeIcon(ic, iconOnly ? bareIcon : btnFont, !iconOnly);
        if (!iconOnly) ic->setMarginRight(6.0f);
        b->addView(ic, 0);   // before the text label
        viewIcons.push_back(ic);

        int idx = (int)i;
        b->registerClickAction([idx](brls::View*) {
            stremio::selectActiveView(idx);
            return true;
        });
        (iconOnly ? rightBar : viewBar)->addView(b);
        viewBtns.push_back(b);
    }

    // An icon-only borderless button, built like the ones above: an IconLabel
    // in front of the Button's own (empty) label, which leaves the style's side
    // padding around the glyph.
    auto makeIconButton = [bareIcon, sizeIcon](const char* glyph) {
        auto* b = new brls::Button();
        b->setStyle(&brls::BUTTONSTYLE_BORDERLESS);
        auto* ic = new IconLabel();
        ic->setText(glyph);
        sizeIcon(ic, bareIcon, false);
        // A bare Label defaults to the app's text colour, not the button's --
        // which is a different value in the light variant.
        ic->setTextColor(
            brls::Application::getTheme()["brls/button/default_enabled_text"]);
        b->addView(ic, 0);
        return b;
    };

    // Material "account_circle": who is signed in to Stremio, and the way out.
    auto* profileBtn = makeIconButton("");
    profileBtn->registerClickAction([](brls::View*) {
        brls::Application::pushActivity(new AccountActivity());
        return true;
    });
    rightBar->addView(profileBtn);

    // Material "settings".
    auto* settingsBtn = makeIconButton("");
    settingsBtn->registerClickAction([](brls::View*) {
        brls::Application::pushActivity(new SettingsActivity());
        return true;
    });
    rightBar->addView(settingsBtn);

    // active >= 0: show the bar and mark that view PRIMARY. active < 0: fold it
    // away and take its buttons out of the focus ring (a GONE box keeps its
    // children focusable otherwise -- the same trap as the sign-in form).
    // The group and the detached Search icon are separate absolute boxes, so the
    // step between them needs saying too.
    viewBtns[searchIdx - 1]->setCustomNavigationRoute(brls::FocusDirection::RIGHT,
                                                     viewBtns[searchIdx]);
    viewBtns[searchIdx]->setCustomNavigationRoute(brls::FocusDirection::LEFT,
                                                  viewBtns[searchIdx - 1]);

    // Where the centred group starts. At the 100% UI size inset it so it
    // centres in the space beside the header icon rather than running into it.
    // Every smaller UI has room to spare, and true centring reads better, so
    // they keep it. The size is stored per mode, hence the docked check -- and
    // hence recomputing this when the console is docked or undocked.
    auto insetViewBar = [viewBar]() {
        bool docked = brls::Application::windowWidth >= 1920;
        int  w      = docked ? config::get().dockedUiWidth
                             : config::get().handheldUiWidth;
        viewBar->setPositionLeft(w <= config::kDefaultHandheldUiWidth ? 120.0f
                                                                     : 0.0f);
    };
    brls::Application::getWindowSizeChangedEvent()->subscribe(insetViewBar);
    // Docking fires the event above; editing the UI size in Options does not
    // (applyUiScale resizes the logical space itself), hence the second route.
    setUiScaleHook(insetViewBar);

    auto curView      = std::make_shared<int>(-1);
    auto applyViewBar = [viewBar, viewBtns, viewIcons, profileBtn, searchIdx,
                         curView, insetViewBar](int active) {
        *curView  = active;
        bool show = active >= 0;
        // Also here, not only on the window event: changing the UI size in
        // Options resizes the logical space without firing that event (see
        // applyUiScale), and this runs on the way back into a view.
        insetViewBar();
        viewBar->setVisibility(show ? brls::Visibility::VISIBLE
                                    : brls::Visibility::GONE);
        // The account button is part of the view bar, so it folds away with it.
        // Options does not: it is always reachable.
        profileBtn->setVisibility(show ? brls::Visibility::VISIBLE
                                       : brls::Visibility::GONE);
        profileBtn->setFocusable(show);
        brls::Theme theme = brls::Application::getTheme();
        for (size_t i = 0; i < viewBtns.size(); i++)
        {
            bool isActive = (int)i == active;
            viewBtns[i]->setFocusable(show);
            viewBtns[i]->setStyle(isActive ? &brls::BUTTONSTYLE_PRIMARY
                                           : &brls::BUTTONSTYLE_BORDERLESS);
            // Search sits outside the bar that gets hidden, so it has to be
            // folded away by itself.
            if (i == searchIdx)
                viewBtns[i]->setVisibility(show ? brls::Visibility::VISIBLE
                                                : brls::Visibility::GONE);
            // The icon lives in its own Label, so it does not inherit the button's
            // per-state text colour on its own -- keep it in step here.
            viewIcons[i]->setTextColor(
                theme[isActive ? "brls/button/primary_enabled_text"
                               : "brls/button/default_enabled_text"]);
        }
        // Up from the Stremio list lands on the ACTIVE view button (Continue /
        // Movies / ...). reportView runs before the list is (re)built, so
        // finishList picks this up. Null while the bar is hidden (the sign-in
        // screen has no list to come up from).
        stremio::setLibraryUpTarget(show && active < (int)viewBtns.size()
                                        ? (brls::View*)viewBtns[active]
                                        : nullptr);
    };
    applyViewBar(-1);
    stremio::setViewTabSink(applyViewBar);

    // Hard-anchor the two bars in the header: the Stremio view switcher centred,
    // Search / account / Options top-right.
    if (auto* header = frame->getHeader())
    {
        // Left AND right pinned: that is what gives an absolute box a width to
        // centre its buttons in -- there is no "centre me" for one. So this box
        // spans the whole header, and it goes in FIRST on purpose: Box::hitTest
        // walks its children in reverse and a box that contains the point
        // returns ITSELF when no child matches, so a full-width box added last
        // swallowed every touch in the header -- the buttons under it stopped
        // responding to the touchscreen. Tested last, it only catches the taps
        // nothing else wanted. (insetViewBar owns its left edge.)
        viewBar->setPositionType(brls::PositionType::ABSOLUTE);
        viewBar->setPositionRight(0.0f);
        viewBar->setPositionTop(20.0f);
        header->addView(viewBar);

        rightBar->setPositionType(brls::PositionType::ABSOLUTE);
        rightBar->setPositionRight(30.0f);
        rightBar->setPositionTop(20.0f);
        header->addView(rightBar);
    }

    // The header bars hold their colours until restyled, so an accent change in
    // Options would otherwise only show up on the next view switch. Restyle in
    // place -- rebuilding the tab from here would free views that are focused,
    // or on the focus stack while Options sits on top of us.
    theme::setRepaintHook(
        [applyViewBar, curView]() { applyViewBar(*curView); });
}

brls::View* buildBrowser()
{
    auto* frame = new BrowserFrame();
    // Our content box is a plain Box with no header of its own, so it goes
    // inside the AppletFrame that carries the title/icon.
    //
    // IMPORTANT: everything touching the header has to happen AFTER
    // pushContentView -- it calls updateAppletFrameItem(), which empties
    // hint_box and resets title/icon to the content view's (blank) ones.
    auto* content = new brls::Box();
    content->setAxis(brls::Axis::COLUMN);
    content->setGrow(1.0f);
    frame->pushContentView(content);
    attachTopTabBar(frame, content);

    // Header text/category chip is disabled.
    stremio::setLibraryCountSink([](const std::string&) {});

    // No X shortcut for Options: the header carries a gear now, and a chip in
    // the footer for something already on screen is noise.

    // R/L cycle the Stremio view. On the frame (not the tab) so they work with
    // focus on the header bar too; a no-op while signed out.
    // One chip "L R  View": L is the auto icon, R is a glyph in the hint text.
    // (The text glyph renders a touch smaller than the icon -- a borealis hint
    // quirk -- but this keeps them side by side; the R action is hidden so it
    // does not add a second chip.)
    frame->registerAction(std::string("  ") + tr("View"), brls::BUTTON_LB, [](brls::View*) {
        stremio::cycleActiveView(-1);
        return true;
    }, false, false, brls::SOUND_NONE);
    frame->registerAction(tr("View"), brls::BUTTON_RB, [](brls::View*) {
        stremio::cycleActiveView(+1);
        return true;
    }, true, false, brls::SOUND_NONE);

    frame->setActionAvailable(brls::BUTTON_LB, true);
    frame->setActionAvailable(brls::BUTTON_RB, true);

    return frame;
}

// Shown on the libnx text console (NOT borealis) when the app was launched in
// applet mode -- that mode caps the heap far below what mpv and the RAM
// streaming window need. Done before borealis/GL is ever set up: bringing the
// whole UI stack up only to tear it back down on exit is what crashed the
// console in applet mode. Blocks on + then returns, so main() exits cleanly.
static void appletModeBlock()
{
    consoleInit(NULL);
    printf("\n  Stremio Switch - full memory required\n\n");
    printf("  This app was launched in applet mode, which caps\n");
    printf("  memory well below what the player and its\n");
    printf("  streaming buffer need.\n\n");
    printf("  Launch it with full memory instead: start it over\n");
    printf("  a game (hold R while opening the game), or use a\n");
    printf("  forwarder.\n\n");
    printf("  Press + to exit.\n");
    consoleUpdate(NULL);

    PadState pad;
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&pad);
    while (appletMainLoop())
    {
        padUpdate(&pad);
        if (padGetButtonsDown(&pad) & HidNpadButton_Plus) break;
        consoleUpdate(NULL);
    }
    consoleExit(NULL);
}

} // namespace

int main(int argc, char* argv[])
{
    // Applet mode caps the heap far below what mpv + the RAM streaming window
    // need. Bail before anything is initialised (a full-RAM launch runs as an
    // Application or SystemApplication; everything else is applet mode) -- and
    // via the plain text console, since standing borealis/GL up just to shut it
    // down on exit is what crashed the console here.
    AppletType appletType = appletGetAppletType();
    if (appletType != AppletType_Application &&
        appletType != AppletType_SystemApplication)
    {
        appletModeBlock();
        return EXIT_SUCCESS;
    }

    ensureAppDataDir();

    config::load();
    // Straight after the config it reads, and before anything can build a view:
    // tr() is resolved when a string is handed to a view, and the header and
    // the browser are built once and never again.
    i18n::load();
    // The engine default already matches the config default, but the config
    // may say otherwise: hand it over before any torrentfs can be opened.
    torrentfs_set_governor(config::get().rateGovernor ? 1 : 0);
    // argv[0] tells us which .nro to replace; nothing else can.
    update::init(argc, argv);

    // Off by default: unbuffered writes plus a [stats] line every 2s means the
    // SD card is written to for the whole session. Opt in from Options when
    // something needs diagnosing.
    brls::Logger::setLogLevel(config::get().logging ? brls::LogLevel::LOG_DEBUG
                                                    : brls::LogLevel::LOG_ERROR);
    if (config::get().logging)
    {
        if (FILE* lf = std::fopen(APPDATA_LOG, "w+"))
        {
            // Async sink (see LogRing above): loggers memcpy into a RAM ring,
            // a dedicated thread does the actual SD writes. Priority 0x3B
            // (well below everything): losing a beat only delays the file,
            // never a frame.
            g_logRing = new LogRing();
            g_logRing->out = lf;
            mutexInit(&g_logRing->lock);
            condvarInit(&g_logRing->cv);
            FILE* rf = nullptr;
            if (threadCreate(&g_logRing->thread, logDrainThread, nullptr,
                             nullptr, 0x8000, 0x3B, -2) == 0)
            {
                threadStart(&g_logRing->thread);
                rf = funopen(g_logRing, nullptr, logRingWrite, nullptr, nullptr);
            }
            if (rf)
            {
                // Unbuffered so every Logger line reaches the ring (and its
                // wake) immediately -- the "write" is a memcpy, so unbuffered
                // costs nothing here.
                std::setvbuf(rf, nullptr, _IONBF, 0);
                brls::Logger::setLogOutput(rf);
            }
            else
            {
                // No thread/funopen: fall back to the old direct unbuffered
                // file rather than losing the log entirely.
                std::setvbuf(lf, nullptr, _IONBF, 0);
                brls::Logger::setLogOutput(lf);
            }
        }
    }

    // Must happen once, before any thread touches curl: the old source/main.c
    // did this, but that front-end is no longer compiled, so curl was being used
    // (Stremio login) without ever being initialised.
    curl_global_init(CURL_GLOBAL_DEFAULT);

    if (!brls::Application::init())
    {
        brls::Logger::error("Unable to init borealis");
        return EXIT_FAILURE;
    }

    brls::Application::createWindow("Stremio Switch");
    // Order matters: SwitchPlatform caches the console's ColorSetId at
    // construction and setThemeVariant() overwrites that same field, so the
    // console's own preference has to be read before applyVariant() runs -- it is
    // unrecoverable afterwards. ("Follow the console" depends on this.)
    theme::captureSystemVariant();
    theme::applyVariant();

    brls::Application::setGlobalQuit(true);

    // Before anything is built: plenty of views read the accent and the palette
    // once, as they are constructed, and keep the colours they got.
    theme::applyAccent();

    // The window was sized inside createWindow, before any activity exists, so
    // fix the logical size now -- the first layout then happens at the user's
    // scale instead of being redone. (config::load() has already run.)
    applyUiScale();
    // Docking/undocking mid-session swaps the framebuffer between 720p and
    // 1080p, and the two modes have their own UI size; follow it.
    brls::Application::getWindowSizeChangedEvent()->subscribe(applyUiScale);

    brls::Application::pushActivity(new brls::Activity(buildBrowser()));

    // A quiet check: it only ever surfaces when there IS a newer release, so a
    // missing network or an unreachable GitHub costs nothing but a log line.
    if (config::get().checkUpdates)
        update::checkAsync([](update::Release r) {
            if (r.ok && r.installable()) update::promptInstall(r);
        });

    while (brls::Application::mainLoop())
        ;

    // Only now: hbloader keeps the running .nro open and libnx reads our romfs
    // out of it, so the file cannot be replaced until the app is done with it.
    // The new version is picked up on the next launch.
    update::applyPending();

    // Nothing to wait for here: ~MpvView closes the engine synchronously, and
    // Application::exit() destroys the activity stack before this returns. That
    // matters because as soon as main() returns libnx runs __appExit ->
    // userAppExit -> socketExit(), and any engine thread still alive past that
    // point would dereference a NULL socket devoptab and data abort.
    return EXIT_SUCCESS;
}
