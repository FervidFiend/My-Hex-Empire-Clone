#include "Game.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <vector>
#include <algorithm>
#include <sstream>
#include <utility>

namespace {
constexpr int kMapWidth = 20;
constexpr int kMapHeight = 11;
constexpr int kTileCount = kMapWidth * kMapHeight;
constexpr int kFactionCount = 4;
constexpr int kMoveLimit = 5;
// Sprite 167 removes itself on frame 12 of its 12 fps timeline.
constexpr int kExplosionFrames = 11;
// Sprite 167's erosion clip copies the dry background through e_6.png when
// its erosion_frame counter reaches 6, then pastes 15 graves on frame 7.
constexpr float kBattleScarDelaySeconds = 5.0f / 12.0f;
constexpr int kBattleScarGraveCount = 15;
constexpr float kBattleScarSize = 170.0f;
// Sprite 70 starts with blink = 6 * bd (bd = 2) and toggles _alpha between
// 100 and 50 on every bd-th frame, so new armies dim three times in a second.
constexpr int kArmyBlinkFrames = 12;
constexpr int kArmyBlinkPeriod = 2;

// Sprite 264's per-frame layer placements, generated from the SWF.
#include "IntroTimeline.inc"
// Sprite 280 plays for 49 frames at 12 fps before frame 50 hides it.
constexpr float kCapitalTooltipDurationSeconds = 49.0f / 12.0f;
// Sprites 211, 213 and 215 (the opening gameplay tooltips) play for 69 frames
// before frame 70 hides them.
constexpr float kOpeningTooltipDurationSeconds = 69.0f / 12.0f;

// Intro is sprite 264, the title sequence ending at START. Preparing covers
// sprite 232 frames 11-20: from picking a capital, through sprite 207's
// "Please wait" modal, to the first turn.
enum class Screen { Intro, Menu, Preparing, Playing, Result };
enum class Estate { None, Town, Port };
enum class NewspaperArticle : size_t {
    Peace = 0,
    Advance,
    VictoryClose,
    Captured,
    ProvinceConquered,
    StrategicRetreat,
    WarNotLost,
    Lost,
    Speech,
    PactSigned,
    Annexed,
    Marika,
    PactBroken
};

constexpr size_t kNewspaperArticleCount = 13;

struct Army {
    bool present = false;
    int owner = -1;
    int manpower = 0;
    int morale = 0;
    bool moved = false;
    // Seconds since sprite 70 was attached; negative once its blink is over.
    float blinkElapsed = -1.0f;
};

struct Tile {
    bool land = false;
    bool dry = false;
    int artVariant = 0;
    int dryArtVariant = 0;
    int artRotation = 0;
    bool artFlipHorizontal = false;
    bool artFlipVertical = false;
    int owner = -1;
    Estate estate = Estate::None;
    int capitalFor = -1;
    std::string townName;
    Army army;
};

struct TerrainPatch {
    int dryLandVariant = 0;
    int landVariant = 0;
    int rotation = 0;
    bool flipHorizontal = false;
    bool flipVertical = false;
};

struct BattleExplosion {
    int tile = -1;
    float elapsed = 0.0f;
    bool scarred = false;
    bool soundPlayed = false;
    // The exploding army clip stays on its field until sprite 167 frame 4
    // hides it. In a won attack it sits above the arriving army; a repelled
    // attacker was put below the defender.
    Army ghost;
    bool ghostOnTop = true;
};

struct BattleScarGrave {
    int variant = 0;
    // Top-left corner in board units, as passed to the SWF's pasteBitmap.
    glm::vec2 position{};
};

struct BattleScar {
    int tile = -1;
    bool flipHorizontal = false;
    bool flipVertical = false;
    int rotation = 0;
    std::array<BattleScarGrave, kBattleScarGraveCount> graves{};
};

// Sprite 435's pages: frame 5 lists the kingdoms, then "fail" or "success".
enum class PactDialogPage { Choose, Refused, Signed };

// Modal panels built from sprite 4 frames: 407 (capitulate), 437 (your turn)
// and 192 (the first-visit tutorial prompt on country selection).
enum class ModalDialogKind { None, Capitulate, YourTurn, Tutorial };

struct GameState {
    Screen screen = Screen::Menu;
    std::array<Tile, kTileCount> tiles{};
    std::array<std::array<float, kFactionCount>, kTileCount> territoryAlpha{};
    std::array<TerrainPatch, 24> terrainPatches{};
    int selectedFaction = 0;
    int previewFaction = -1;
    int human = 0;
    int currentPlayer = 0;
    int movesLeft = 0;
    // The SWF resolves attack() when the move is ordered, so a defender that
    // repels the attack shows its losses while the attacker is still sliding.
    int repelledDefenderTile = -1;
    Army repelledDefender;
    // END TURN pressed while an army travels: finish the move, then end.
    bool endTurnRequested = false;
    // Seconds since the campaign started (the board frame clip's timeline).
    float gameplayElapsed = 0.0f;
    // Sprite 232's timeline: seconds since "generate" (frames 1-10, held on
    // frame 6 while sprite 192 is open), and since leaving country selection
    // (frames 11-20, held on frame 13 while sprite 207 runs).
    float menuElapsed = 0.0f;
    float preparingElapsed = 0.0f;
    int preparingFaction = 0;
    // Seconds since sprite 207 was attached; negative before it is.
    float pleaseWaitElapsed = -1.0f;
    // Sprite 264: its timeline frame (held on 42 until START) and the frames
    // run since it started, which drive its children's own loops.
    int introFrame = 1;
    int introTicks = 0;
    float introFrameTime = 0.0f;
    bool introStartPressed = false;
    // foto_2 and foto_1 (sprite 254): stage x, start x, step, _alpha, photo.
    struct IntroPhoto { float x = 0.0f; float startX = 0.0f; float step = 0.0f; int alpha = 0; int photo = 0; };
    std::array<IntroPhoto, 2> introPhotos{};
    // Seconds since sprite 231 (the game-over panel) was attached.
    float resultElapsed = 0.0f;
    // board.win: the human took a capital while holding two provinces.
    bool humanConquered = false;
    int turns = 0;
    int humanTurnStartArea = 0;
    int newsPriority = 0;
    bool collectingMoveNews = false;
    unsigned int newsSerial = 0;
    int movingArmyFrom = -1;
    int movingArmyTarget = -1;
    int queuedArmyFrom = -1;
    int queuedArmyTarget = -1;
    float armyTravelElapsed = 0.0f;
    float aiMovePause = 0.0f;
    std::array<int, kFactionCount> aiSupportTargets{ -1, -1, -1, -1 };
    std::array<int, kFactionCount> aiSupportWaitCounts{};
    int selectedTile = -1;
    int hoveredTile = -1;
    std::array<float, kTileCount> armyInfoAlpha{};
    bool armySelectionHintVisible = false;
    // Sprite 61 plays on from frame 2 once the hinted army is clicked; this
    // counts seconds since then (negative while inactive).
    float armyMoveHintElapsed = -1.0f;
    int armyHintTile = -1;
    // !_root.ts_army_hint: show the army hint when the human's turn starts.
    bool armyHintPending = false;
    bool movesLeftHintVisible = false;
    bool specialButtonsHintVisible = false;
    bool helpHintVisible = false;
    float openingTooltipSecondsRemaining = 0.0f;
    std::array<float, kTileCount> cityNameAlpha{};
    int pactPartner = -1;
    PactDialogPage pactDialogPage = PactDialogPage::Choose;
    // hw_pact_signed: set once any pact succeeds. Sprite 81 frame 4 only
    // applies it to the button when the human's turn starts.
    bool pactSigned = false;
    bool pactButtonInactive = false;
    ModalDialogKind modalDialog = ModalDialogKind::None;
    int modalDialogPage = 0;
    // Seconds since the dialog was attached, and since each sprite 43 on the
    // current page was placed (they fade in over their first four frames).
    float modalDialogElapsed = 0.0f;
    std::array<float, 2> modalButtonElapsed{};
    // _root.ts_your_turn and ts_choose_country: shown once per session.
    bool yourTurnShown = false;
    bool tutorialPromptShown = false;
    int difficulty = 5;
    // Each field's `doom` (sprite 281): battles add 20 to their field and 10
    // to its neighbours, and it never decays.
    std::array<float, kTileCount> fieldDoom{};
    int mapNumber = 408739;
    // CHANGE MAP with an empty field: _root.map_number = "" passes the
    // `>= 0` test (AS2 computes !("" < 0), and NaN < 0 is undefined), so
    // setSeed("") leaves rnd_seed NaN and every rand() returns NaN.
    bool blankMapNumber = false;
    std::unique_ptr<TextInputBox> mapNumberInput;
    std::array<std::unique_ptr<Button>, 6> selectionButtons;
    std::array<std::unique_ptr<Button>, 4> gameplayActionButtons;
    bool showHelp = false;
    int helpPage = 0;
    float capitalTooltipSecondsRemaining = kCapitalTooltipDurationSeconds;
    bool capitalTooltipTimerStarted = false;
    std::array<float, kFactionCount> factionShieldScales{ 0.75f, 0.75f, 0.75f, 0.75f };
    std::array<float, kFactionCount> gameplayBadgeScales{ 0.75f, 0.75f, 0.75f, 0.75f };
    std::array<float, kFactionCount> gameplayBadgeOpacities{ 0.75f, 0.75f, 0.75f, 0.75f };
    bool choosingPact = false;
    bool speechUsed = false;
    bool showSpeechDialog = false;
    bool playerWon = false;
    std::vector<int> moveTargets;
    std::vector<BattleExplosion> explosions;
    // Permanent terrain damage, replayed into the GPU damage layer in order.
    std::vector<BattleScar> battleScars;
    unsigned int battleScarGeneration = 0;
    std::vector<std::string> news;
    NewspaperArticle newspaperArticle = NewspaperArticle::Peace;
    std::mt19937 random{ std::random_device{}() };
    // Cosmetic draws (the SWF's unseeded rand) must not disturb the map seed.
    std::mt19937 effectsRandom{ std::random_device{}() };
};

GameState game;

using TextureSet = std::array<std::unique_ptr<Texture>, 6>;

struct GameArtwork {
    std::array<std::unique_ptr<Texture>, kExplosionFrames> explosionFrames;
    TextureSet land;
    TextureSet dryLand;
    TextureSet towns;
    TextureSet dryTowns;
    TextureSet sea;
    std::unique_ptr<Texture> erosionMask;
    std::array<std::unique_ptr<Texture>, 6> graves;
    std::unique_ptr<Texture> capitalMarkerShadow;
    std::array<std::unique_ptr<Texture>, 4> capitalDiscs;
    std::unique_ptr<Texture> capitalStarOutline;
    std::array<std::unique_ptr<Texture>, 4> capitalStars;
    std::array<std::unique_ptr<Texture>, 4> factionShields;
    std::array<std::unique_ptr<Texture>, kFactionCount> conqueredCapitalShields;
    std::unique_ptr<Texture> heraldicLions;
    std::unique_ptr<Texture> humanCrossedSwords;
    std::unique_ptr<Texture> pactPeaceEmblem;
    std::unique_ptr<Texture> armyManpowerMoraleBadge;
    std::array<std::array<std::unique_ptr<Texture>, kFactionCount>, 3> armyLandUnits;
    std::array<std::array<std::unique_ptr<Texture>, kFactionCount>, 3> armyWaterUnits;
    float armyUnitTextureScale = -1.0f;
    std::unique_ptr<Texture> factionShieldShadowBezel;
    std::unique_ptr<Texture> factionShieldHighlight;
    // Shape 87: the half-black outline sprite 91 draws over every shield.
    std::unique_ptr<Texture> factionShieldOutline;
    std::unique_ptr<Texture> capitalNameRule;
    std::array<std::unique_ptr<Texture>, 2> seaPorts;
    std::unique_ptr<Texture> royalCrown;
    std::map<int, std::unique_ptr<Texture>> newspaperShapes;
    std::array<std::unique_ptr<Texture>, 5> townMarkerFallbacks;
    std::unique_ptr<Texture> portMedallion;
    std::unique_ptr<Texture> hoveredHex;
    std::unique_ptr<Texture> hexGridOutline;
    std::unique_ptr<Texture> parchment;
    std::unique_ptr<Texture> newspaperPaper;
    std::unique_ptr<Texture> pactDialog;
    // Sprite 435's "fail" and "success" panels without the modal.
    std::unique_ptr<Texture> pactRefusedPanel;
    std::unique_ptr<Texture> pactSignedPanel;
    std::unique_ptr<Texture> capitulatePanel;
    std::array<std::unique_ptr<Texture>, 3> yourTurnPanels;
    std::array<std::unique_ptr<Texture>, 3> tutorialPanels;
    // Sprite 189 frame 2, the "(e.g. ...)" capital pin on tutorial pages 2-3.
    std::unique_ptr<Texture> tutorialCapitalPin;
    // Sprite 207's panel and its sprite 206 gauge frames, and sprite 80: the
    // black "Unbelievably complex calculations" screen covering map generation.
    std::unique_ptr<Texture> pleaseWaitPanel;
    std::array<std::unique_ptr<Texture>, 100> loadingGauge;
    std::unique_ptr<Texture> mapGenerationScreen;
    // Sprite 264's layers; see IntroTimeline.inc.
    std::unique_ptr<Texture> introBackdrop;
    std::unique_ptr<Texture> introBanner;
    // Sprite 259's lettering, masking its glint and turning fire glow.
    std::array<std::unique_ptr<Texture>, kIntroTextLoopFrames> introText;
    std::unique_ptr<Texture> introSponsorLogo;
    std::unique_ptr<Texture> introSpiral;
    std::unique_ptr<Texture> introMetaSauce;
    std::unique_ptr<Texture> introMoreGames;
    std::unique_ptr<Texture> introSkyline;
    std::unique_ptr<Texture> introVignette;
    std::unique_ptr<Texture> introAppStore;
    std::array<std::unique_ptr<Texture>, 13> introCrest;
    std::array<std::unique_ptr<Texture>, 13> introBlurredCrest;
    std::array<std::unique_ptr<Texture>, 8> introPhotos;
    std::array<std::unique_ptr<Texture>, 4> helpPages;
    std::unique_ptr<Texture> armySelectionTooltip;
    std::unique_ptr<Texture> movesLeftTooltip;
    std::unique_ptr<Texture> specialButtonsTooltip;
    std::unique_ptr<Texture> helpTooltip;
    std::unique_ptr<Texture> helpTooltipShadow;
    std::unique_ptr<Texture> armySelectionTooltipShadow;
    std::unique_ptr<Texture> redButton;
    std::unique_ptr<Texture> silverButton;
    std::unique_ptr<Texture> redButtonHover;
    std::unique_ptr<Texture> silverButtonHover;
    std::unique_ptr<Texture> soundOnIcon;
    std::unique_ptr<Texture> soundOffIcon;
    std::unique_ptr<Texture> capitalTooltip;
    std::unique_ptr<Texture> speechDialogFrame;
    std::unique_ptr<Texture> speechDialogPhoto;
    std::unique_ptr<Texture> gameplayMapFrame;
    std::unique_ptr<Texture> gameplayMapEdge;
    std::unique_ptr<Texture> boardCornerShadow;
    std::unique_ptr<Texture> armyMoveTooltip;
    // Pieces of the default newspaper (sprite 354, frame 1) and its paper
    // backing (shape 296), all from the board's frame clip.
    std::unique_ptr<Texture> newspaperBacking;
    std::unique_ptr<Texture> newspaperColumnRules;
    std::unique_ptr<Texture> newspaperTextColumn;
    std::unique_ptr<Texture> newspaperPhoto;
    std::unique_ptr<Texture> newspaperRules;
    std::unique_ptr<Font> newspaperBoldDigitsFont;
    std::unique_ptr<Texture> gameplayStageBorder;
    std::unique_ptr<Texture> gameplaySkyline;
    std::unique_ptr<Texture> mapControlsOutline;
    std::unique_ptr<Font> newspaperTitleFont;
    std::unique_ptr<Font> selectionFont;
    std::unique_ptr<Font> arialNarrowFont;
    std::unique_ptr<Font> armyCounterFont;
    std::unique_ptr<Font> armyBadgeFont;
    // Font 174, used by sprite 231's `result` field.
    std::unique_ptr<Font> resultFont;
    // Sprite 231's "lost" and "win" panels without the modal or result text.
    std::unique_ptr<Texture> resultPanelLost;
    std::unique_ptr<Texture> resultPanelWin;
    std::unique_ptr<Font> mapLabelFont;
    std::unique_ptr<Font> mapInputFont;
    std::unique_ptr<Font> audioToggleFont;
    std::unique_ptr<Font> helpIconFont;
};

GameArtwork artwork;

struct GameAudio {
    std::unique_ptr<SoundEffect> buttonClick;
    std::unique_ptr<SoundEffect> fieldDeselect;
    std::unique_ptr<SoundEffect> armyMove;
    std::unique_ptr<SoundEffect> battleWar;
    std::unique_ptr<SoundEffect> battleGun;
    std::unique_ptr<SoundEffect> explosion;
    // Sound 252: sprite 264 starts it looping on frame 2 and stops it on 47.
    std::unique_ptr<MusicSound> introMusic;
    std::unique_ptr<MusicSound> happyMusic;
    std::unique_ptr<MusicSound> doomMusic;
    bool muted = false;
    bool loaded = false;
    bool gameplayTracksStarted = false;
    // The SWF eases s_happy and s_doom independently, so they need not sum to
    // 100. Flash keeps Sound volumes as whole percentages, which the mixer
    // emulates by stepping these at 12 fps.
    int happyVolume = 100;
    int doomVolume = 0;
    float mixerFrameTime = 0.0f;
};

GameAudio gameAudio;

void SetGameAudioMuted(bool muted) {
    gameAudio.muted = muted;
    if (extras::openAL && extras::openAL->openAlLoaded)
        alListenerf(AL_GAIN, muted ? 0.0f : 1.0f);
}

void LoadGameAudio() {
    if (gameAudio.loaded || !extras::openAL || !extras::openAL->openAlLoaded) return;
    gameAudio.loaded = true;

    try {
        gameAudio.buttonClick = std::make_unique<SoundEffect>(
            "resources/game/sounds/sfx_blop_id440.mp3", 8);
    } catch (const std::exception& error) {
        std::cerr << "Could not load the SWF button sound: " << error.what() << std::endl;
    }
    try {
        // Field clicks that drop a selection play s_bup at volume 25.
        gameAudio.fieldDeselect = std::make_unique<SoundEffect>(
            "resources/game/sounds/sfx_bump_id439.mp3", 8);
        gameAudio.fieldDeselect->setMaxVolume(0.25f);
    } catch (const std::exception& error) {
        std::cerr << "Could not load the SWF deselect sound: " << error.what() << std::endl;
    }
    auto loadEffect = [](std::unique_ptr<SoundEffect>& target, const char* path, float volume) {
        try {
            target = std::make_unique<SoundEffect>(path, 8);
            target->setMaxVolume(volume);
        } catch (const std::exception& error) {
            std::cerr << "Could not load game sound " << path << ": " << error.what() << std::endl;
        }
    };
    // Volumes follow the SWF's playSound/createSound calls: s_szur 25,
    // s_war and s_gun 100, s_explosion_n 15.
    loadEffect(gameAudio.armyMove, "resources/game/sounds/sfx_scrape_id438.mp3", 0.25f);
    loadEffect(gameAudio.battleWar, "resources/game/sounds/sfx_war_id442.mp3", 1.0f);
    loadEffect(gameAudio.battleGun, "resources/game/sounds/sfx_gunshot_id441.mp3", 1.0f);
    loadEffect(gameAudio.explosion, "resources/game/sounds/sfx_explosion_id443.mp3", 0.15f);

    auto loadMusic = [](std::unique_ptr<MusicSound>& target, const char* path) {
        try {
            target = std::make_unique<MusicSound>(path, true);
        } catch (const std::exception& error) {
            std::cerr << "Could not load game music " << path << ": " << error.what() << std::endl;
        }
    };
    loadMusic(gameAudio.happyMusic, "resources/game/sounds/sfx_happy_id444.mp3");
    loadMusic(gameAudio.doomMusic, "resources/game/sounds/sfx_doom_id446.mp3");
    loadMusic(gameAudio.introMusic, "resources/game/sounds/unlabeled_sound_id252.mp3");
    // s_happy starts with the board (sprite 81 frame 1), after the intro.
    if (gameAudio.doomMusic) gameAudio.doomMusic->setVolume(0.0f);
    SetGameAudioMuted(false);
}

// Sprite 81 frame 1 runs continueSound(_root.s_happy) as the first map is
// generated, once the intro has finished.
void StartMenuMusic() {
    if (gameAudio.happyMusic) {
        gameAudio.happyMusic->setVolume(1.0f);
        gameAudio.happyMusic->play();
    }
}

void StartGameplayMusic() {
    gameAudio.gameplayTracksStarted = true;
    gameAudio.happyVolume = 100;
    gameAudio.doomVolume = 0;
    gameAudio.mixerFrameTime = 0.0f;
    if (gameAudio.happyMusic) {
        gameAudio.happyMusic->setVolume(1.0f);
        gameAudio.happyMusic->play();
    }
    if (gameAudio.doomMusic) {
        gameAudio.doomMusic->setVolume(0.0f);
        gameAudio.doomMusic->play();
    }
}

void UpdateGameplayMusic(float deltaTime) {
    if (!gameAudio.gameplayTracksStarted) return;

    // Each 12 fps frame runs setVolume(getVolume() + (target - getVolume()) / 20).
    // Flash drops the fraction, so a rising volume stalls once the gap is under
    // 20 while a falling one still reaches its target. That keeps s_doom quiet
    // over lightly fought fields instead of layering it over s_happy.
    const auto easeTo = [](int targetHappy, int targetDoom) {
        gameAudio.happyVolume = static_cast<int>(gameAudio.happyVolume +
            (targetHappy - gameAudio.happyVolume) / 20.0f);
        gameAudio.doomVolume = static_cast<int>(gameAudio.doomVolume +
            (targetDoom - gameAudio.doomVolume) / 20.0f);
    };
    constexpr float frameSeconds = 1.0f / 12.0f;
    gameAudio.mixerFrameTime = std::min(gameAudio.mixerFrameTime + std::max(0.0f, deltaTime),
        frameSeconds * 12.0f);
    for (; gameAudio.mixerFrameTime >= frameSeconds; gameAudio.mixerFrameTime -= frameSeconds) {
        if (game.screen == Screen::Playing) {
            // Sprite 281's enterFrame only mixes while the pointer is over a land
            // field, toward that field's doom. Elsewhere the volumes hold.
            const int hovered = game.hoveredTile;
            if (hovered >= 0 && hovered < kTileCount && game.tiles[static_cast<size_t>(hovered)].land) {
                const int doom = static_cast<int>(std::min(game.fieldDoom[static_cast<size_t>(hovered)], 100.0f));
                easeTo(100 - doom, doom);
            }
        } else if (game.screen == Screen::Result) {
            // Sprite 231's frame 1 mixer runs before it branches to win or lose,
            // so both endings fade into s_doom.
            easeTo(0, 100);
        } else {
            easeTo(100, 0);
        }
    }
    if (gameAudio.happyMusic) gameAudio.happyMusic->setVolume(gameAudio.happyVolume / 100.0f);
    if (gameAudio.doomMusic) gameAudio.doomMusic->setVolume(gameAudio.doomVolume / 100.0f);
}

void PlayButtonClickSound() {
    if (gameAudio.buttonClick) gameAudio.buttonClick->play();
}

void PlayFieldDeselectSound() {
    if (gameAudio.fieldDeselect) gameAudio.fieldDeselect->play();
}

glm::vec4 FactionColor(int faction);
glm::vec4 FactionTerritoryColor(int faction);
void DrawGameplayNewspaper(int width, int height);

std::unique_ptr<Texture> DownsampleSupersampledSvg(const Texture& source);

constexpr float kArmyUnitCanvasWidth = 34.45f;
constexpr float kArmyUnitCanvasHeight = 27.2f;
constexpr float kArmyUnitFilterPadding = 7.0f;
constexpr float kArmyUnitRasterScale = 4.0f;

std::unique_ptr<Texture> BuildFactionUnitTexture(const Texture& source, size_t faction, float displayScale) {
    const size_t sourceSize = static_cast<size_t>(source.width) * source.height * 4;
    if (source.width <= 0 || source.height <= 0 || source.textureData.size() < sourceSize)
        return nullptr;

    // These are the four ColorMatrixFilters on sprite_70's army clip in the
    // SWF (frames 2-5, selected with party + 2). The source sprite is green;
    // Flash uses these matrices to turn its shaded pixels into each party's
    // actual unit palette. Replacing every pixel with the territory RGB loses
    // the pale highlights and darker edges visible in the original.
    static constexpr std::array<std::array<float, 20>, kFactionCount> colorMatrices = {{
        {{-0.279886782f, 7.18070269f, -5.34081554f, 0.0f, -177.519974f,
          -0.579860747f, 0.254226387f, 1.88563442f, 0.0f, -177.519989f,
          4.96976614f, -1.35259473f, -2.05717134f, 0.0f, -177.519989f,
          0.0f, 0.0f, 0.0f, 1.0f, 0.0f}},
        {{-2.78354549f, 1.90435481f, 3.47919083f, 0.0f, -249.800003f,
          1.50419414f, 1.64641094f, -0.550604999f, 0.0f, -249.800018f,
          -1.59052932f, 6.66290712f, -2.47237802f, 0.0f, -249.800003f,
          0.0f, 0.0f, 0.0f, 1.0f, 0.0f}},
        {{0.976864517f, -2.16930914f, 3.25244474f, 0.0f, -182.669998f,
          0.360050291f, 2.69862723f, -0.998677433f, 0.0f, -182.669998f,
          -2.90464330f, 2.85059571f, 2.11404729f, 0.0f, -182.669983f,
          0.0f, 0.0f, 0.0f, 1.0f, 0.0f}},
        {{4.43134642f, 4.11569309f, -6.56703901f, 0.0f, -216.670029f,
          -1.97042978f, 2.21131492f, 1.73911476f, 0.0f, -216.669998f,
          4.40836716f, -5.69256449f, 3.26419735f, 0.0f, -216.669998f,
          0.0f, 0.0f, 0.0f, 1.0f, 0.0f}}
    }};

    faction = std::min(faction, colorMatrices.size() - 1);
    const auto& matrix = colorMatrices[faction];
    // Keep NanoSVG's 4x raster for vector coverage, but resolve that coverage
    // before running the SWF filter chain. Flash/Ruffle draw the vector into a
    // display-sized render target, then run ColorMatrix, Glow, and DropShadow
    // as separate filters. Filtering the 4x bitmap and shrinking the glow with
    // the art makes the small unit's colored fills and outline look washed out.
    const int filterPadding = static_cast<int>(std::lround(
        kArmyUnitFilterPadding * displayScale));
    const int width = std::max(1, static_cast<int>(std::lround(
        (kArmyUnitCanvasWidth + 2.0f * kArmyUnitFilterPadding) * displayScale)));
    const int height = std::max(1, static_cast<int>(std::lround(
        (kArmyUnitCanvasHeight + 2.0f * kArmyUnitFilterPadding) * displayScale)));
    const int contentWidth = std::max(1, width - filterPadding * 2);
    const int contentHeight = std::max(1, height - filterPadding * 2);
    const size_t pixelCount = static_cast<size_t>(width) * height;
    std::vector<float> alpha(pixelCount, 0.0f);
    std::vector<float> rgb(pixelCount * 3, 0.0f);

    for (int y = 0; y < contentHeight; ++y) {
        const float sourceTop = static_cast<float>(y) * source.height / contentHeight;
        const float sourceBottom = static_cast<float>(y + 1) * source.height / contentHeight;
        const int firstSourceY = std::clamp(
            static_cast<int>(std::floor(sourceTop)), 0, source.height);
        const int lastSourceY = std::clamp(
            static_cast<int>(std::ceil(sourceBottom)), 0, source.height);
        for (int x = 0; x < contentWidth; ++x) {
            const float sourceLeft = static_cast<float>(x) * source.width / contentWidth;
            const float sourceRight = static_cast<float>(x + 1) * source.width / contentWidth;
            const int firstSourceX = std::clamp(
                static_cast<int>(std::floor(sourceLeft)), 0, source.width);
            const int lastSourceX = std::clamp(
                static_cast<int>(std::ceil(sourceRight)), 0, source.width);
            float alphaSum = 0.0f;
            float premultipliedColor[3] = {};
            for (int sourceY = firstSourceY; sourceY < lastSourceY; ++sourceY) {
                const float weightY = std::max(0.0f,
                    std::min(sourceBottom, static_cast<float>(sourceY + 1)) -
                    std::max(sourceTop, static_cast<float>(sourceY)));
                for (int sourceX = firstSourceX; sourceX < lastSourceX; ++sourceX) {
                    const float weightX = std::max(0.0f,
                        std::min(sourceRight, static_cast<float>(sourceX + 1)) -
                        std::max(sourceLeft, static_cast<float>(sourceX)));
                    const float weight = weightX * weightY;
                    const size_t sourcePixel =
                        (static_cast<size_t>(sourceY) * source.width + sourceX) * 4;
                    const float sourceAlpha = source.textureData[sourcePixel + 3] / 255.0f;
                    alphaSum += sourceAlpha * weight;
                    for (size_t channel = 0; channel < 3; ++channel) {
                        premultipliedColor[channel] +=
                            source.textureData[sourcePixel + channel] * sourceAlpha * weight;
                    }
                }
            }

            const float area = (sourceRight - sourceLeft) * (sourceBottom - sourceTop);
            const float sourceAlpha = area > 0.0f ? alphaSum / area : 0.0f;
            const size_t pixel = static_cast<size_t>(y + filterPadding) * width +
                x + filterPadding;
            float sourceChannels[4] = {};
            if (alphaSum > 0.0f) {
                for (size_t channel = 0; channel < 3; ++channel)
                    sourceChannels[channel] = premultipliedColor[channel] / alphaSum / 255.0f;
            }
            sourceChannels[3] = sourceAlpha;

            // The SWF matrix is defined over unmultiplied RGB values in the
            // 0..1 range, with its byte offsets normalized by 255.
            for (size_t channel = 0; channel < 3; ++channel) {
                const size_t row = channel * 5;
                float color = matrix[row + 4] / 255.0f;
                for (size_t input = 0; input < 4; ++input)
                    color += matrix[row + input] * sourceChannels[input];
                rgb[pixel * 3 + channel] =
                    std::round(std::clamp(color, 0.0f, 1.0f) * 255.0f);
            }
            float outputAlpha = matrix[19] / 255.0f;
            for (size_t input = 0; input < 4; ++input)
                outputAlpha += matrix[15 + input] * sourceChannels[input];
            alpha[pixel] = std::round(std::clamp(outputAlpha, 0.0f, 1.0f) * 255.0f) /
                255.0f;
        }
    }

    // The SWF uses one pass for each filter; keep blur and offset values in
    // the same native pixels as the extracted sprite frame.
    const auto sampleAlphaBilinear = [width, height](const std::vector<float>& image,
        float sampleX, float sampleY) {
        const int x0 = static_cast<int>(std::floor(sampleX));
        const int y0 = static_cast<int>(std::floor(sampleY));
        const float fx = sampleX - x0;
        const float fy = sampleY - y0;
        const auto at = [&image, width, height](int x, int y) {
            if (x < 0 || x >= width || y < 0 || y >= height) return 0.0f;
            return image[static_cast<size_t>(y) * width + x];
        };
        const float top = at(x0, y0) * (1.0f - fx) + at(x0 + 1, y0) * fx;
        const float bottom = at(x0, y0 + 1) * (1.0f - fx) + at(x0 + 1, y0 + 1) * fx;
        return top * (1.0f - fy) + bottom * fy;
    };
    // Flash-compatible renderers use a fractional box blur, not an
    // integer-sized box kernel. The two edge samples can have fractional
    // weight; channel precision is resolved to 8 bits between axes.
    const auto blurAlphaAxis = [width, height, &sampleAlphaBilinear](
        const std::vector<float>& input, float fullSize, bool horizontal) {
        if (fullSize <= 1.0f) return input;

        const float radius = (fullSize - 1.0f) * 0.5f;
        const float middlePairs = std::ceil(radius) - 1.0f;
        const float edgeWeight = std::floor((radius - middlePairs) * 255.0f) / 255.0f;
        if (edgeWeight <= 0.0f) return input;
        const float lastOffset = 1.0f / (1.0f / edgeWeight + 1.0f);
        const float lastWeight = edgeWeight + 1.0f;
        std::vector<float> output(input.size(), 0.0f);

        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                const float centerX = static_cast<float>(x);
                const float centerY = static_cast<float>(y);
                const auto sampleAt = [&](float offset) {
                    return horizontal
                        ? sampleAlphaBilinear(input, centerX - middlePairs + offset, centerY)
                        : sampleAlphaBilinear(input, centerX, centerY - middlePairs + offset);
                };

                float sum = sampleAt(-1.0f) * edgeWeight;
                for (float pair = 0.5f; pair < middlePairs * 2.0f; pair += 2.0f)
                    sum += sampleAt(pair) * 2.0f;
                sum += sampleAt(middlePairs * 2.0f + lastOffset) * lastWeight;

                const float blurred = std::clamp(sum / fullSize, 0.0f, 1.0f);
                output[static_cast<size_t>(y) * width + x] =
                    std::floor(blurred * 255.0f) / 255.0f;
            }
        }
        return output;
    };
    const auto blurAlpha = [&blurAlphaAxis](const std::vector<float>& input,
        float blurX, float blurY) {
        const std::vector<float> horizontal = blurAlphaAxis(input, blurX, true);
        return blurAlphaAxis(horizontal, blurY, false);
    };
    const float glowBlurSize = 3.0f * displayScale;
    const std::vector<float> glowBlur = blurAlpha(alpha, glowBlurSize, glowBlurSize);
    for (size_t pixel = 0; pixel < pixelCount; ++pixel) {
        // Flash's GlowFilter uses the strength-scaled blurred alpha as the
        // glow mask, then composites the original source over it. Source
        // opacity already masks the glow during that composite; subtracting
        // it from the blurred mask as well makes antialiased edges too dim.
        const float glowMaskAlpha = std::clamp(glowBlur[pixel] * 10.0f, 0.0f, 1.0f);
        const float glowAlpha = glowMaskAlpha * (1.0f - alpha[pixel]);
        if (glowAlpha <= 0.0f) continue;
        const float outputAlpha = std::round(
            std::min(1.0f, alpha[pixel] + glowAlpha) * 255.0f) / 255.0f;
        for (size_t channel = 0; channel < 3; ++channel) {
            const float sourcePremultiplied = rgb[pixel * 3 + channel] * alpha[pixel];
            const float outputPremultiplied = std::round(std::clamp(
                sourcePremultiplied + 255.0f * glowAlpha, 0.0f, 255.0f));
            rgb[pixel * 3 + channel] = outputAlpha > 0.0f
                ? outputPremultiplied / outputAlpha : 0.0f;
        }
        alpha[pixel] = outputAlpha;
    }

    const float shadowBlurSize = 4.0f * displayScale;
    const std::vector<float> shadowBlur = blurAlpha(alpha, shadowBlurSize, shadowBlurSize);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const size_t pixel = static_cast<size_t>(y) * width + x;
            // SWF angle 0.785385 radians and distance 2 put the shadow down
            // and right. Texture rows are bottom-up, so screen-down samples
            // from a higher texture row. The outer shadow sits behind the
            // source-plus-glow composite.
            const float shadowOffset = 1.4142f * displayScale;
            const float shiftedShadow = sampleAlphaBilinear(shadowBlur,
                x - shadowOffset, y + shadowOffset);
            const float shadowAlpha = std::clamp(shiftedShadow, 0.0f, 1.0f) *
                (1.0f - alpha[pixel]);
            if (shadowAlpha <= 0.0f) continue;
            const float outputAlpha = std::round(
                std::min(1.0f, alpha[pixel] + shadowAlpha) * 255.0f) / 255.0f;
            for (size_t channel = 0; channel < 3; ++channel) {
                const float sourcePremultiplied = rgb[pixel * 3 + channel] * alpha[pixel];
                rgb[pixel * 3 + channel] = outputAlpha > 0.0f
                    ? std::round(std::clamp(sourcePremultiplied, 0.0f, 255.0f)) /
                        outputAlpha : 0.0f;
            }
            alpha[pixel] = outputAlpha;
        }
    }

    std::vector<unsigned char> filteredPixels(pixelCount * 4, 0);
    for (size_t pixel = 0; pixel < pixelCount; ++pixel) {
        const float pixelAlpha = std::clamp(alpha[pixel], 0.0f, 1.0f);
        for (size_t channel = 0; channel < 3; ++channel) {
            filteredPixels[pixel * 4 + channel] = static_cast<unsigned char>(
                std::lround(std::clamp(rgb[pixel * 3 + channel] * pixelAlpha, 0.0f, 255.0f)));
        }
        filteredPixels[pixel * 4 + 3] = static_cast<unsigned char>(
            std::lround(pixelAlpha * 255.0f));
    }

    auto filteredTexture = std::make_unique<Texture>(width, height, std::move(filteredPixels));
    filteredTexture->updateSamplerDetails(GL_FilterModes::NEAREST, GL_WrapModes::CLAMP_TO_EDGE);
    return filteredTexture;
}

struct SwfDropShadowStyle {
    float blurX = 5.0f;
    float blurY = 5.0f;
    float angleRadians = 0.7853851318359375f;
    float distance = 2.0f;
    float strength = 1.0f;
    std::array<unsigned char, 4> color = { 0, 0, 0, 255 };
    int passes = 1;
};

// The SWF uses this same DropShadowFilter on the tooltip bubble children in
// sprites 61 and 215. Its text children have no filter.
constexpr SwfDropShadowStyle kSwfTooltipDropShadow{};

std::unique_ptr<Texture> BuildSwfDropShadow(const Texture& mask,
    const SwfDropShadowStyle& filter) {
    const int width = mask.width;
    const int height = mask.height;
    const size_t pixelCount = static_cast<size_t>(width) * height;
    if (width <= 0 || height <= 0 || mask.textureData.size() < pixelCount * 4)
        return nullptr;

    // Apply the SWF filter to the bubble alpha mask only. Drawing this below
    // the source bubble reproduces compositeSource=true and knockout=false.
    constexpr float rasterScale = 4.0f;
    const int blurWindowX = std::max(1, static_cast<int>(std::lround(
        std::max(0.0f, filter.blurX) * rasterScale)));
    const int blurWindowY = std::max(1, static_cast<int>(std::lround(
        std::max(0.0f, filter.blurY) * rasterScale)));
    const float offsetX = std::cos(filter.angleRadians) * filter.distance * rasterScale;
    const float offsetY = std::sin(filter.angleRadians) * filter.distance * rasterScale;
    const float colorAlpha = filter.color[3] / 255.0f;

    std::vector<float> sourceAlpha(pixelCount, 0.0f);
    for (size_t pixel = 0; pixel < pixelCount; ++pixel)
        sourceAlpha[pixel] = mask.textureData[pixel * 4 + 3] / 255.0f;

    const auto sampleAlphaBilinear = [width, height](const std::vector<float>& image,
        float sampleX, float sampleY) {
        const int x0 = static_cast<int>(std::floor(sampleX));
        const int y0 = static_cast<int>(std::floor(sampleY));
        const float fx = sampleX - x0;
        const float fy = sampleY - y0;
        const auto at = [&image, width, height](int x, int y) {
            if (x < 0 || x >= width || y < 0 || y >= height) return 0.0f;
            return image[static_cast<size_t>(y) * width + x];
        };
        const float top = at(x0, y0) * (1.0f - fx) + at(x0 + 1, y0) * fx;
        const float bottom = at(x0, y0 + 1) * (1.0f - fx) + at(x0 + 1, y0 + 1) * fx;
        return top * (1.0f - fy) + bottom * fy;
    };

    const auto blurAxis = [width, height, &sampleAlphaBilinear](
        const std::vector<float>& input, bool horizontal, int blurWindow) {
        std::vector<float> output(input.size(), 0.0f);
        const float firstOffset = -0.5f * (blurWindow - 1);
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                float sum = 0.0f;
                for (int sample = 0; sample < blurWindow; ++sample) {
                    const float offset = firstOffset + static_cast<float>(sample);
                    sum += sampleAlphaBilinear(input,
                        horizontal ? x + offset : static_cast<float>(x),
                        horizontal ? static_cast<float>(y) : y + offset);
                }
                output[static_cast<size_t>(y) * width + x] = sum / blurWindow;
            }
        }
        return output;
    };
    std::vector<float> blurred = sourceAlpha;
    for (int pass = 0; pass < std::max(1, filter.passes); ++pass) {
        const std::vector<float> horizontalBlur = blurAxis(blurred, true, blurWindowX);
        blurred = blurAxis(horizontalBlur, false, blurWindowY);
    }

    std::vector<unsigned char> pixels(pixelCount * 4, 0);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const size_t pixel = static_cast<size_t>(y) * width + x;
            // Texture rows are bottom-up after SVG loading, while SWF screen
            // coordinates grow downward. The positive SWF angle therefore
            // needs the opposite sign on the texture-row sample coordinate.
            const float alpha = std::clamp(sampleAlphaBilinear(blurred,
                x - offsetX, y + offsetY) * filter.strength * colorAlpha, 0.0f, 1.0f);
            pixels[pixel * 4] = filter.color[0];
            pixels[pixel * 4 + 1] = filter.color[1];
            pixels[pixel * 4 + 2] = filter.color[2];
            pixels[pixel * 4 + 3] = static_cast<unsigned char>(std::lround(alpha * 255.0f));
        }
    }

    auto shadow = std::make_unique<Texture>(width, height, std::move(pixels));
    // The generated image is still 4x; mipmaps resolve that coverage when
    // it is drawn at the SWF's native logical size.
    shadow->updateSamplerDetails(GL_FilterModes::MIPMAP, GL_WrapModes::CLAMP_TO_EDGE);
    return shadow;
}

void DrawTooltipShadow(Texture& shadow, const Rect& contentBounds,
    float contentWidth, float contentHeight, float opacity = 1.0f) {
    constexpr float rasterScale = 4.0f;
    const float shadowWidth = shadow.width / rasterScale;
    const float shadowHeight = shadow.height / rasterScale;
    const float paddingX = (shadowWidth - contentWidth) * 0.5f;
    const float paddingY = (shadowHeight - contentHeight) * 0.5f;
    DrawRectangle(Rect(contentBounds.position.x - paddingX *
            (contentBounds.size.x / contentWidth),
        contentBounds.position.y - paddingY * (contentBounds.size.y / contentHeight),
        shadowWidth * (contentBounds.size.x / contentWidth),
        shadowHeight * (contentBounds.size.y / contentHeight)),
        glm::vec4(1.0f, 1.0f, 1.0f, std::clamp(opacity, 0.0f, 1.0f)), shadow);
}

std::unique_ptr<Texture> DownsampleSupersampledSvg(const Texture& source) {
    // Keep this in sync with Texture::loadFromFile's NanoSVG 4x raster scale.
    constexpr int rasterScale = 4;
    if (source.width < rasterScale || source.height < rasterScale) return nullptr;

    const int width = source.width / rasterScale;
    const int height = source.height / rasterScale;
    const size_t sourcePixelCount = static_cast<size_t>(source.width) * source.height;
    if (source.textureData.size() < sourcePixelCount * 4) return nullptr;

    std::vector<unsigned char> pixels(static_cast<size_t>(width) * height * 4, 0);
    constexpr float sampleCount = static_cast<float>(rasterScale * rasterScale);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            float alphaSum = 0.0f;
            float premultipliedColor[3] = { 0.0f, 0.0f, 0.0f };
            // NanoSVG returns straight RGBA. Average premultiplied color and
            // coverage, then unpremultiply to preserve translucent edge hues.
            for (int sampleY = 0; sampleY < rasterScale; ++sampleY) {
                for (int sampleX = 0; sampleX < rasterScale; ++sampleX) {
                    const int sourceX = x * rasterScale + sampleX;
                    const int sourceY = y * rasterScale + sampleY;
                    const size_t sourcePixel =
                        (static_cast<size_t>(sourceY) * source.width + sourceX) * 4;
                    const float alpha = source.textureData[sourcePixel + 3] / 255.0f;
                    alphaSum += alpha;
                    for (size_t channel = 0; channel < 3; ++channel)
                        premultipliedColor[channel] +=
                            source.textureData[sourcePixel + channel] * alpha;
                }
            }

            const size_t destinationPixel =
                (static_cast<size_t>(y) * width + x) * 4;
            if (alphaSum > 0.0f) {
                for (size_t channel = 0; channel < 3; ++channel)
                    pixels[destinationPixel + channel] = static_cast<unsigned char>(
                        std::lround(premultipliedColor[channel] / alphaSum));
            }
            pixels[destinationPixel + 3] = static_cast<unsigned char>(
                std::lround(alphaSum / sampleCount * 255.0f));
        }
    }

    auto nativeSize = std::make_unique<Texture>(width, height, std::move(pixels));
    nativeSize->updateSamplerDetails(GL_FilterModes::LINEAR, GL_WrapModes::CLAMP_TO_EDGE);
    return nativeSize;
}

std::unique_ptr<Texture> BuildButtonHoverMask(const Texture& source) {
    const size_t expectedSize = static_cast<size_t>(source.width) * source.height * 4;
    if (source.width <= 0 || source.height <= 0 || source.textureData.size() < expectedSize)
        return nullptr;

    std::vector<unsigned char> pixels = source.textureData;
    for (size_t pixel = 0; pixel < expectedSize; pixel += 4) {
        pixels[pixel] = 255;
        pixels[pixel + 1] = 255;
        pixels[pixel + 2] = 255;
    }
    auto mask = std::make_unique<Texture>(source.width, source.height, std::move(pixels));
    mask->updateSamplerDetails(GL_FilterModes::LINEAR, GL_WrapModes::CLAMP_TO_EDGE);
    return mask;
}

void LoadFactionUnitTextures(std::array<std::unique_ptr<Texture>, kFactionCount>& destination,
    const char* path, float displayScale) {
    Texture source(path, true, false, false,
        kArmyUnitRasterScale * displayScale);
    if (source.width <= 0 || source.height <= 0) return;

    for (size_t faction = 0; faction < destination.size(); ++faction)
        destination[faction] = BuildFactionUnitTexture(source, faction, displayScale);
}

void EnsureArmyUnitTexturesForScale(float displayScale) {
    if (!std::isfinite(displayScale) || displayScale <= 0.0f) return;
    if (std::abs(artwork.armyUnitTextureScale - displayScale) < 0.0001f) return;

    static constexpr std::array<const char*, 3> landUnitFrames = {{
        "resources/game/sprites/sprite_69_soldier_icon/vector_frame_001.svg",
        "resources/game/sprites/sprite_69_soldier_icon/vector_frame_002.svg",
        "resources/game/sprites/sprite_69_soldier_icon/vector_frame_003.svg"
    }};
    static constexpr const char* waterUnitFrame =
        "resources/game/sprites/sprite_69_soldier_icon/vector_frame_006.svg";
    for (size_t variant = 0; variant < landUnitFrames.size(); ++variant) {
        LoadFactionUnitTextures(artwork.armyLandUnits[variant],
            landUnitFrames[variant], displayScale);
        LoadFactionUnitTextures(artwork.armyWaterUnits[variant],
            waterUnitFrame, displayScale);
    }
    artwork.armyUnitTextureScale = displayScale;
}

void LoadTextureSet(TextureSet& destination, const std::array<const char*, 6>& files) {
    for (size_t i = 0; i < files.size(); ++i) {
        auto texture = std::make_unique<Texture>();
        texture->loadFromFile(std::string("resources/game/images/") + files[i], true, false);
        if (texture->width > 0 && texture->height > 0)
            destination[i] = std::move(texture);
    }
}

void PrepareTransparentMapTextures(TextureSet& textures) {
    // Preserve each sprite's alpha coverage while extending nearby opaque
    // colors into its transparent border. This removes the green matte RGB
    // stored in some source pixels before linear filtering samples the coast.
    for (const auto& texture : textures) {
        if (!texture || texture->width <= 0 || texture->height <= 0) continue;

        const size_t expectedSize = static_cast<size_t>(texture->width) *
            static_cast<size_t>(texture->height) * 4;
        if (texture->textureData.size() >= expectedSize) {
            const std::vector<unsigned char> source = texture->textureData;
            constexpr int bleedRadius = 3;
            const int width = texture->width;
            const int height = texture->height;
            for (int y = 0; y < height; ++y) {
                for (int x = 0; x < width; ++x) {
                    const size_t pixel = (static_cast<size_t>(y) * width + x) * 4;
                    // Preserve authored semi-transparent edge colors: they
                    // participate in the source's land/sea color blend.
                    // Only bleed color into fully transparent texels to avoid
                    // filtering halos from their unused RGB values.
                    if (source[pixel + 3] != 0) continue;

                    int nearestDistance = bleedRadius * bleedRadius + 1;
                    size_t nearestPixel = expectedSize;
                    for (int offsetY = -bleedRadius; offsetY <= bleedRadius; ++offsetY) {
                        const int sampleY = y + offsetY;
                        if (sampleY < 0 || sampleY >= height) continue;
                        for (int offsetX = -bleedRadius; offsetX <= bleedRadius; ++offsetX) {
                            const int sampleX = x + offsetX;
                            if (sampleX < 0 || sampleX >= width) continue;
                            const int distance = offsetX * offsetX + offsetY * offsetY;
                            if (distance == 0 || distance >= nearestDistance) continue;

                            const size_t samplePixel =
                                (static_cast<size_t>(sampleY) * width + sampleX) * 4;
                            if (source[samplePixel + 3] < 240) continue;
                            nearestDistance = distance;
                            nearestPixel = samplePixel;
                        }
                    }

                    if (nearestPixel < expectedSize) {
                        texture->textureData[pixel] = source[nearestPixel];
                        texture->textureData[pixel + 1] = source[nearestPixel + 1];
                        texture->textureData[pixel + 2] = source[nearestPixel + 2];
                    }
                }
            }
            texture->update(texture->textureData.data());
        }
        texture->updateSamplerDetails(GL_FilterModes::LINEAR, GL_WrapModes::CLAMP_TO_EDGE);
    }
}

void LoadGameArtwork() {
    for (int frame = 0; frame < kExplosionFrames; ++frame) {
        auto texture = std::make_unique<Texture>();
        const std::string number = std::to_string(frame + 1);
        texture->loadFromFile("resources/game/sprites/sprite_167_explosion_animation/frame_" +
            std::string(3 - number.size(), '0') + number + ".png", true, false);
        if (texture->width <= 0 || texture->height <= 0) continue;
        // The export includes the red script-holder placeholder on frames 1
        // and 11. It is not explosion artwork; clear its isolated corner.
        if (frame == 0 || frame == 10) {
            for (int y = 17; y <= 44 && y < texture->height; ++y)
                for (int x = 8; x <= 35 && x < texture->width; ++x)
                    texture->textureData[(static_cast<size_t>(texture->height - 1 - y) * texture->width + x) * 4 + 3] = 0;
            texture->update(texture->textureData.data());
        }
        texture->updateSamplerDetails(GL_FilterModes::LINEAR, GL_WrapModes::CLAMP_TO_EDGE);
        artwork.explosionFrames[frame] = std::move(texture);
    }

    // LegacyMapRandom returns 0..5 for the source's rand(6)+1 sequence,
    // so index 0 must resolve to the asset named variant_01.
    LoadTextureSet(artwork.land, {
        "land_map_tile_variant_01_id379.png", "land_map_tile_variant_02_id378.png", "land_map_tile_variant_03_id377.png",
        "land_map_tile_variant_04_id376.png", "land_map_tile_variant_05_id375.png", "land_map_tile_variant_06_id374.png"
    });
    LoadTextureSet(artwork.dryLand, {
        "dry_land_map_tile_variant_01_id373.png", "dry_land_map_tile_variant_02_id372.png", "dry_land_map_tile_variant_03_id371.png",
        "dry_land_map_tile_variant_04_id370.png", "dry_land_map_tile_variant_05_id369.png", "dry_land_map_tile_variant_06_id368.png"
    });
    LoadTextureSet(artwork.towns, {
        "town_map_tile_variant_01_id403.png", "town_map_tile_variant_02_id402.png", "town_map_tile_variant_03_id401.png",
        "town_map_tile_variant_04_id400.png", "town_map_tile_variant_05_id399.png", "town_map_tile_variant_06_id398.png"
    });
    LoadTextureSet(artwork.dryTowns, {
        "dry_town_map_tile_variant_01_id397.png", "dry_town_map_tile_variant_02_id396.png", "dry_town_map_tile_variant_03_id395.png",
        "dry_town_map_tile_variant_04_id394.png", "dry_town_map_tile_variant_05_id393.png", "dry_town_map_tile_variant_06_id392.png"
    });
    LoadTextureSet(artwork.sea, {
        "sea_map_tile_variant_01_id391.png", "sea_map_tile_variant_02_id390.png", "sea_map_tile_variant_03_id389.png",
        "sea_map_tile_variant_04_id388.png", "sea_map_tile_variant_05_id387.png", "sea_map_tile_variant_06_id386.png"
    });
    PrepareTransparentMapTextures(artwork.land);
    PrepareTransparentMapTextures(artwork.dryLand);
    PrepareTransparentMapTextures(artwork.towns);
    PrepareTransparentMapTextures(artwork.dryTowns);
    PrepareTransparentMapTextures(artwork.sea);
    // e_6.png is the erosion clip's copyPixels alpha mask; n_1..n_6 are graves.
    artwork.erosionMask = std::make_unique<Texture>();
    artwork.erosionMask->loadFromFile("resources/game/images/yellow_burst_effect_frame_06_id140.png", true, false);
    if (artwork.erosionMask->width <= 0 || artwork.erosionMask->height <= 0)
        artwork.erosionMask.reset();
    else
        artwork.erosionMask->updateSamplerDetails(GL_FilterModes::LINEAR, GL_WrapModes::CLAMP_TO_EDGE);
    static constexpr std::array<const char*, 6> graveFiles = {
        "small_effect_frame_01_id365.png", "small_effect_frame_02_id364.png", "small_effect_frame_03_id363.png",
        "small_effect_frame_04_id362.png", "small_effect_frame_05_id361.png", "small_effect_frame_06_id360.png"
    };
    for (size_t i = 0; i < graveFiles.size(); ++i) {
        auto grave = std::make_unique<Texture>();
        grave->loadFromFile(std::string("resources/game/images/") + graveFiles[i], true, false);
        if (grave->width > 0 && grave->height > 0) {
            grave->updateSamplerDetails(GL_FilterModes::LINEAR, GL_WrapModes::CLAMP_TO_EDGE);
            artwork.graves[i] = std::move(grave);
        }
    }
    // The SWF copies town pixels without smoothing at native size. Preserve
    // that crisp sampling for rotated city sprites, but blend mip levels when
    // the map is reduced to limit aliasing.
    for (const TextureSet* textures : { &artwork.towns, &artwork.dryTowns }) {
        for (const auto& texture : *textures) {
            if (texture)
                texture->updateSamplerDetails(GL_FilterModes::MIPMAP_NEAREST, GL_WrapModes::CLAMP_TO_EDGE);
        }
    }
    // The SWF assembles sea tiles with BitmapData.copyPixels and attaches the
    // result as a non-smoothed bitmap. Keep these sprites pixel-exact while
    // building the matching 800x600 composite.
    for (const auto& texture : artwork.sea) {
        if (texture)
            texture->updateSamplerDetails(GL_FilterModes::NEAREST, GL_WrapModes::CLAMP_TO_EDGE);
    }
    artwork.hoveredHex = std::make_unique<Texture>(
        "resources/game/shapes/light_gray_hexagon_fill_id272.svg", true, false);
    if (artwork.hoveredHex->width <= 0 || artwork.hoveredHex->height <= 0)
        artwork.hoveredHex.reset();
    artwork.hexGridOutline = std::make_unique<Texture>(
        "resources/game/shapes/gray_hexagon_outline_id268.svg", true, false);
    if (artwork.hexGridOutline->width <= 0 || artwork.hexGridOutline->height <= 0)
        artwork.hexGridOutline.reset();
    artwork.capitalMarkerShadow = std::make_unique<Texture>(
        "resources/game/shapes/blue_city_selection_marker_id179.svg", true, false, true);
    if (artwork.capitalMarkerShadow->width <= 0 || artwork.capitalMarkerShadow->height <= 0)
        artwork.capitalMarkerShadow.reset();
    const std::array<const char*, 4> discFiles = {
        "capital_disc_red_id180.svg", "capital_disc_violet_id183.svg",
        "capital_disc_blue_id185.svg", "capital_disc_green_id187.svg"
    };
    const std::array<const char*, 4> starFiles = {
        "capital_star_red_id182.svg", "capital_star_violet_id184.svg",
        "capital_star_blue_id186.svg", "capital_star_green_id188.svg"
    };
    for (size_t faction = 0; faction < discFiles.size(); ++faction) {
        artwork.capitalDiscs[faction] = std::make_unique<Texture>(
            std::string("resources/game/shapes/") + discFiles[faction], true, false, true);
        artwork.capitalStars[faction] = std::make_unique<Texture>(
            std::string("resources/game/shapes/") + starFiles[faction], true, false, true);
        if (artwork.capitalDiscs[faction]->width <= 0 || artwork.capitalDiscs[faction]->height <= 0)
            artwork.capitalDiscs[faction].reset();
        if (artwork.capitalStars[faction]->width <= 0 || artwork.capitalStars[faction]->height <= 0)
            artwork.capitalStars[faction].reset();
    }
    artwork.capitalStarOutline = std::make_unique<Texture>(
        "resources/game/shapes/capital_star_outline_id181.svg", true, false, true);
    if (artwork.capitalStarOutline->width <= 0 || artwork.capitalStarOutline->height <= 0)
        artwork.capitalStarOutline.reset();
    const std::array<const char*, 4> shieldFiles = {
        "faction_shield_red_id86.svg", "faction_shield_violet_id88.svg",
        "faction_shield_blue_id89.svg", "faction_shield_green_id90.svg"
    };
    for (size_t faction = 0; faction < artwork.factionShields.size(); ++faction) {
        artwork.factionShields[faction] = std::make_unique<Texture>(
            std::string("resources/game/shapes/") + shieldFiles[faction], true, false, true);
        if (artwork.factionShields[faction]->width <= 0 || artwork.factionShields[faction]->height <= 0)
            artwork.factionShields[faction].reset();

        auto capitalShield = std::make_unique<Texture>();
        capitalShield->loadFromFile("resources/game/sprites/sprite_91_red_shield_piece/frame_00" +
            std::to_string(faction + 1) + ".png", true, false);
        if (capitalShield->width > 0 && capitalShield->height > 0) {
            capitalShield->updateSamplerDetails(GL_FilterModes::LINEAR, GL_WrapModes::CLAMP_TO_EDGE);
            artwork.conqueredCapitalShields[faction] = std::move(capitalShield);
        }
    }
    artwork.heraldicLions = std::make_unique<Texture>();
    artwork.heraldicLions->loadFromFile(
        "resources/game/images/royal_crest_horse_figures_id82.png", true, false);
    if (artwork.heraldicLions->width <= 0 || artwork.heraldicLions->height <= 0)
        artwork.heraldicLions.reset();
    else
        artwork.heraldicLions->updateSamplerDetails(GL_FilterModes::LINEAR, GL_WrapModes::CLAMP_TO_EDGE);
    // Shapes 98 and 100 are sprite 99 (human) and sprite 101 (peace).
    for (auto [texture, path] : { std::pair{ &artwork.humanCrossedSwords, "resources/game/shapes/crossed_swords_yellow_id98.svg" },
             std::pair{ &artwork.pactPeaceEmblem, "resources/game/shapes/dove_olive_branch_yellow_id100.svg" },
             std::pair{ &artwork.factionShieldOutline, "resources/game/shapes/faction_shield_neutral_outline_id87.svg" } }) {
        *texture = std::make_unique<Texture>(path, true, false, true);
        if ((*texture)->width <= 0 || (*texture)->height <= 0)
            texture->reset();
    }
    artwork.armyManpowerMoraleBadge = std::make_unique<Texture>(
        "resources/game/shapes/diagonal_red_split_circle_id47.svg", true, false, true);
    if (artwork.armyManpowerMoraleBadge->width <= 0 ||
        artwork.armyManpowerMoraleBadge->height <= 0)
        artwork.armyManpowerMoraleBadge.reset();
    else
        artwork.armyManpowerMoraleBadge->updateSamplerDetails(
            GL_FilterModes::LINEAR, GL_WrapModes::CLAMP_TO_EDGE);
    // Army SVGs are rasterized on demand at the board scale so the vector
    // artwork and SWF filters resolve together at their final display size.
    artwork.factionShieldShadowBezel = std::make_unique<Texture>(
        "resources/game/shapes/black_shield_star_emblem_id85.svg", true, false, true);
    if (artwork.factionShieldShadowBezel->width <= 0 || artwork.factionShieldShadowBezel->height <= 0)
        artwork.factionShieldShadowBezel.reset();
    artwork.factionShieldHighlight = std::make_unique<Texture>(
        "resources/game/shapes/white_fade_overlay_id92.svg", true, false, true);
    if (artwork.factionShieldHighlight->width <= 0 || artwork.factionShieldHighlight->height <= 0)
        artwork.factionShieldHighlight.reset();
    artwork.capitalNameRule = std::make_unique<Texture>(
        "resources/game/shapes/thin_black_horizontal_rule_id108.svg", true, false);
    if (artwork.capitalNameRule->width <= 0 || artwork.capitalNameRule->height <= 0)
        artwork.capitalNameRule.reset();

    const std::array<const char*, 2> seaPortFiles = {
        "sea_port_variant_01_id385.png", "sea_port_variant_02_id384.png"
    };
    for (size_t i = 0; i < artwork.seaPorts.size(); ++i) {
        artwork.seaPorts[i] = std::make_unique<Texture>();
        // These stamps are alpha-composited into the straight-alpha ocean
        // bitmap on the CPU, so keep their source RGB un-premultiplied.
        artwork.seaPorts[i]->loadFromFile(std::string("resources/game/images/") + seaPortFiles[i], true, false);
        if (artwork.seaPorts[i]->width <= 0 || artwork.seaPorts[i]->height <= 0)
            artwork.seaPorts[i].reset();
        else
            artwork.seaPorts[i]->updateSamplerDetails(GL_FilterModes::NEAREST, GL_WrapModes::CLAMP_TO_EDGE);
    }
    artwork.royalCrown = std::make_unique<Texture>();
    artwork.royalCrown->loadFromFile("resources/game/images/royal_crown_id83.png", true, false);
    if (artwork.royalCrown->width <= 0 || artwork.royalCrown->height <= 0)
        artwork.royalCrown.reset();
    else
        artwork.royalCrown->updateSamplerDetails(GL_FilterModes::LINEAR, GL_WrapModes::CLAMP_TO_EDGE);
    for (int frame = 1; frame <= 5; ++frame) {
        auto marker = std::make_unique<Texture>();
        marker->loadFromFile("resources/game/sprites/sprite_189_blue_selection_marker/frame_00" +
            std::to_string(frame) + ".png", true, false);
        if (marker->width > 0 && marker->height > 0) {
            if (frame == 1)
                marker->updateSamplerDetails(GL_FilterModes::NEAREST, GL_WrapModes::CLAMP_TO_EDGE);
            artwork.townMarkerFallbacks[static_cast<size_t>(frame - 1)] = std::move(marker);
        }
    }

    artwork.portMedallion = std::make_unique<Texture>();
    artwork.portMedallion->loadFromFile(
        "resources/game/sprites/sprite_275_blue_faction_medallion/frame_001.png", true, false);
    if (artwork.portMedallion->width <= 0 || artwork.portMedallion->height <= 0)
        artwork.portMedallion.reset();
    else
        artwork.portMedallion->updateSamplerDetails(GL_FilterModes::NEAREST, GL_WrapModes::CLAMP_TO_EDGE);

    artwork.parchment = std::make_unique<Texture>();
    artwork.parchment->loadFromFile("resources/game/images/parchment_background_id103.png", true, false);
    if (artwork.parchment->width <= 0 || artwork.parchment->height <= 0)
        artwork.parchment.reset();
    else
        // Flash clips this bitmap fill; repeating would wrap the opaque
        // bottom row into the tilted footer paper's top edge.
        artwork.parchment->updateSamplerDetails(GL_FilterModes::LINEAR, GL_WrapModes::CLAMP_TO_EDGE);

    artwork.newspaperPaper = std::make_unique<Texture>();
    artwork.newspaperPaper->loadFromFile(
        "resources/game/images/newspaper_paper_texture.png", true, false);
    if (artwork.newspaperPaper->width <= 0 || artwork.newspaperPaper->height <= 0)
        artwork.newspaperPaper.reset();

    artwork.pactDialog = std::make_unique<Texture>();
    artwork.pactDialog->loadFromFile(
        "resources/game/sprites/sprite_435_sign_pact_dialog/frame_005.png", true, false);
    if (artwork.pactDialog->width <= 0 || artwork.pactDialog->height <= 0)
        artwork.pactDialog.reset();

    for (size_t page = 0; page < artwork.helpPages.size(); ++page) {
        auto texture = std::make_unique<Texture>();
        // Sprite 426 frames 5-8, cleaned of the exporter's guide lines by
        // tools/generate_result_panels.py. Page 2's baked blue pin is erased
        // too; the red pin is drawn over it.
        texture->loadFromFile("resources/game/images/help_page_" + std::to_string(page + 1) +
            "_id426.png", true, false);
        if (texture->width > 0 && texture->height > 0)
            artwork.helpPages[page] = std::move(texture);
    }

    artwork.redButton = std::make_unique<Texture>();
    artwork.redButton->loadFromFile(
        "resources/game/buttons/button_34_red_rounded_button/visual_state_01.png", true, false);
    if (artwork.redButton->width <= 0 || artwork.redButton->height <= 0)
        artwork.redButton.reset();

    artwork.silverButton = std::make_unique<Texture>();
    artwork.silverButton->loadFromFile(
        "resources/game/buttons/button_39_silver_rounded_button/visual_state_01.png", true, false);
    if (artwork.silverButton->width <= 0 || artwork.silverButton->height <= 0)
        artwork.silverButton.reset();
    // These SWF shapes contain native 98x32, non-smoothed bitmaps. Linear
    // filtering at the authored fractional placements blurs them a second time.
    for (Texture* button : { artwork.redButton.get(), artwork.silverButton.get() }) {
        if (button)
            button->updateSamplerDetails(GL_FilterModes::NEAREST, GL_WrapModes::CLAMP_TO_EDGE);
    }
    if (artwork.redButton)
        artwork.redButtonHover = BuildButtonHoverMask(*artwork.redButton);
    if (artwork.silverButton)
        artwork.silverButtonHover = BuildButtonHoverMask(*artwork.silverButton);

    artwork.soundOnIcon = std::make_unique<Texture>();
    artwork.soundOnIcon->loadFromFile(
        "resources/game/sprites/sprite_292_sprite_component/frame_001.png", true, false);
    if (artwork.soundOnIcon->width <= 0 || artwork.soundOnIcon->height <= 0)
        artwork.soundOnIcon.reset();
    else
        artwork.soundOnIcon->updateSamplerDetails(GL_FilterModes::NEAREST, GL_WrapModes::CLAMP_TO_EDGE);
    artwork.soundOffIcon = std::make_unique<Texture>();
    artwork.soundOffIcon->loadFromFile(
        "resources/game/sprites/sprite_292_sprite_component/frame_005.png", true, false);
    if (artwork.soundOffIcon->width <= 0 || artwork.soundOffIcon->height <= 0)
        artwork.soundOffIcon.reset();
    else
        artwork.soundOffIcon->updateSamplerDetails(GL_FilterModes::NEAREST, GL_WrapModes::CLAMP_TO_EDGE);

    artwork.gameplayMapFrame = std::make_unique<Texture>(
        "resources/game/shapes/white_hexagon_overlay_id282.svg", true, false);
    if (artwork.gameplayMapFrame->width <= 0 || artwork.gameplayMapFrame->height <= 0)
        artwork.gameplayMapFrame.reset();
    artwork.gameplayMapEdge = std::make_unique<Texture>(
        "resources/game/shapes/hex_map_perimeter_outline.svg", true, false);
    if (artwork.gameplayMapEdge->width <= 0 || artwork.gameplayMapEdge->height <= 0)
        artwork.gameplayMapEdge.reset();
    auto loadShapePng = [](std::unique_ptr<Texture>& texture, const char* name) {
        texture = std::make_unique<Texture>();
        texture->loadFromFile(std::string("resources/game/shapes/") + name, true, false);
        if (texture->width <= 0 || texture->height <= 0) texture.reset();
        else texture->updateSamplerDetails(GL_FilterModes::LINEAR, GL_WrapModes::CLAMP_TO_EDGE);
    };
#include "NewspaperAssets.inc"
    loadShapePng(artwork.newspaperBacking, "parchment_hex_edge_footer_id296.png");
    loadShapePng(artwork.newspaperColumnRules, "paired_vertical_border_lines_a_id297.png");
    loadShapePng(artwork.newspaperTextColumn, "brick_wall_pattern_id298.png");
    loadShapePng(artwork.newspaperPhoto, "black_white_tank_photo_id300.png");
    loadShapePng(artwork.newspaperRules, "horizontal_border_line_set_id304.png");
    // Shape 284 in the board's frame clip: a soft black shadow outside the
    // board's top-left corner, plus another copy of the perimeter stroke.
    artwork.boardCornerShadow = std::make_unique<Texture>(
        "resources/game/shapes/torn_paper_label_frame_id284.svg", true, false);
    if (artwork.boardCornerShadow->width <= 0 || artwork.boardCornerShadow->height <= 0)
        artwork.boardCornerShadow.reset();
    artwork.gameplayStageBorder = std::make_unique<Texture>(
        "resources/game/shapes/gray_transparent_overlay_id251.svg", true, false);
    if (artwork.gameplayStageBorder->width <= 0 || artwork.gameplayStageBorder->height <= 0)
        artwork.gameplayStageBorder.reset();
    artwork.gameplaySkyline = std::make_unique<Texture>(
        "resources/game/shapes/magenta_gradient_overlay_id253.svg", true, false);
    if (artwork.gameplaySkyline->width <= 0 || artwork.gameplaySkyline->height <= 0)
        artwork.gameplaySkyline.reset();
    artwork.mapControlsOutline = std::make_unique<Texture>(
        "resources/game/shapes/soft_gray_edge_overlay_id197.svg", true, false);
    if (artwork.mapControlsOutline->width <= 0 || artwork.mapControlsOutline->height <= 0)
        artwork.mapControlsOutline.reset();
    artwork.capitalTooltip = std::make_unique<Texture>();
    artwork.capitalTooltip->loadFromFile(
        "resources/game/sprites/sprite_280_choose_capital_tooltip/frame_001.png", true, false);
    if (artwork.capitalTooltip->width <= 0 || artwork.capitalTooltip->height <= 0)
        artwork.capitalTooltip.reset();

    artwork.armySelectionTooltip = std::make_unique<Texture>();
    artwork.armySelectionTooltip->loadFromFile(
        "resources/game/shapes/army_selection_tooltip_id61.svg", true, false);
    if (artwork.armySelectionTooltip->width > 0 && artwork.armySelectionTooltip->height > 0)
        artwork.armySelectionTooltip = DownsampleSupersampledSvg(*artwork.armySelectionTooltip);
    if (!artwork.armySelectionTooltip || artwork.armySelectionTooltip->width <= 0 ||
        artwork.armySelectionTooltip->height <= 0)
        artwork.armySelectionTooltip.reset();

    artwork.armyMoveTooltip = std::make_unique<Texture>();
    artwork.armyMoveTooltip->loadFromFile(
        "resources/game/shapes/army_move_tooltip_id61_frame10.svg", true, false);
    if (artwork.armyMoveTooltip->width > 0 && artwork.armyMoveTooltip->height > 0)
        artwork.armyMoveTooltip = DownsampleSupersampledSvg(*artwork.armyMoveTooltip);
    if (!artwork.armyMoveTooltip || artwork.armyMoveTooltip->width <= 0 ||
        artwork.armyMoveTooltip->height <= 0)
        artwork.armyMoveTooltip.reset();

    Texture armySelectionTooltipShadowMask;
    armySelectionTooltipShadowMask.loadFromFile(
        "resources/game/shapes/army_selection_tooltip_shadow_mask_id61.svg", true, false);
    if (armySelectionTooltipShadowMask.width > 0 && armySelectionTooltipShadowMask.height > 0)
        artwork.armySelectionTooltipShadow = BuildSwfDropShadow(
            armySelectionTooltipShadowMask, kSwfTooltipDropShadow);

    artwork.movesLeftTooltip = std::make_unique<Texture>();
    artwork.movesLeftTooltip->loadFromFile(
        "resources/game/sprites/sprite_211_moves_left_tooltip/frame_001.png", true, false);
    if (artwork.movesLeftTooltip->width <= 0 || artwork.movesLeftTooltip->height <= 0)
        artwork.movesLeftTooltip.reset();

    artwork.specialButtonsTooltip = std::make_unique<Texture>();
    artwork.specialButtonsTooltip->loadFromFile(
        "resources/game/sprites/sprite_213_special_buttons_tooltip/frame_001.png", true, false);
    if (artwork.specialButtonsTooltip->width <= 0 || artwork.specialButtonsTooltip->height <= 0)
        artwork.specialButtonsTooltip.reset();

    artwork.helpTooltip = std::make_unique<Texture>();
    artwork.helpTooltip->loadFromFile(
        "resources/game/sprites/sprite_215_help_tooltip/frame_001.png", true, false);
    if (artwork.helpTooltip->width <= 0 || artwork.helpTooltip->height <= 0)
        artwork.helpTooltip.reset();

    Texture helpTooltipShadowMask;
    helpTooltipShadowMask.loadFromFile(
        "resources/game/shapes/help_tooltip_shadow_mask_id215.svg", true, false);
    if (helpTooltipShadowMask.width > 0 && helpTooltipShadowMask.height > 0)
        artwork.helpTooltipShadow = BuildSwfDropShadow(
            helpTooltipShadowMask, kSwfTooltipDropShadow);

    artwork.speechDialogFrame = std::make_unique<Texture>();
    artwork.speechDialogFrame->loadFromFile(
        "resources/game/sprites/sprite_4_game_panel_frame/frame_001.png", true, false);
    if (artwork.speechDialogFrame->width <= 0 || artwork.speechDialogFrame->height <= 0)
        artwork.speechDialogFrame.reset();
    artwork.speechDialogPhoto = std::make_unique<Texture>();
    artwork.speechDialogPhoto->loadFromFile(
        "resources/game/images/archival_war_photo_soldier_statue_id237.png", true, false);
    if (artwork.speechDialogPhoto->width <= 0 || artwork.speechDialogPhoto->height <= 0)
        artwork.speechDialogPhoto.reset();

    artwork.selectionFont = std::make_unique<Font>("resources/game/fonts/54_Adobe Caslon Pro.ttf");
    if (!artwork.selectionFont->font)
        artwork.selectionFont.reset();
    if (artwork.selectionFont)
        artwork.selectionFont->baseGlyphSize = 96;
    artwork.newspaperTitleFont = std::make_unique<Font>("resources/game/fonts/302_Potsdam.ttf");
    if (!artwork.newspaperTitleFont->font)
        artwork.newspaperTitleFont.reset();
    if (artwork.newspaperTitleFont)
        artwork.newspaperTitleFont->baseGlyphSize = 96;
    artwork.arialNarrowFont = std::make_unique<Font>("resources/game/fonts/35_Arial Narrow.ttf");
    if (!artwork.arialNarrowFont->font)
        artwork.arialNarrowFont.reset();
    if (artwork.arialNarrowFont)
        artwork.arialNarrowFont->baseGlyphSize = 96;
    // Font 45 is the separate Arial counter. The map badge's white count and
    // red morale fields both use embedded font 48, Times New Roman Bold.
    // Font 307 is the Adobe Caslon Pro Bold subset (digits) used for moves.
    artwork.newspaperBoldDigitsFont = std::make_unique<Font>("resources/game/fonts/307_Adobe Caslon Pro.ttf");
    if (!artwork.newspaperBoldDigitsFont->font)
        artwork.newspaperBoldDigitsFont.reset();
    if (artwork.newspaperBoldDigitsFont)
        artwork.newspaperBoldDigitsFont->baseGlyphSize = 96;
    artwork.armyCounterFont = std::make_unique<Font>("resources/game/fonts/45_Arial.ttf");
    if (!artwork.armyCounterFont->font)
        artwork.armyCounterFont.reset();
    if (artwork.armyCounterFont)
        artwork.armyCounterFont->baseGlyphSize = 96;
    artwork.armyBadgeFont = std::make_unique<Font>("resources/game/fonts/48_Times New Roman Bold.ttf");
    if (!artwork.armyBadgeFont->font)
        artwork.armyBadgeFont.reset();
    if (artwork.armyBadgeFont)
        artwork.armyBadgeFont->baseGlyphSize = 96;
    artwork.resultFont = std::make_unique<Font>("resources/game/fonts/174_FairfaxStation.ttf");
    if (!artwork.resultFont->font)
        artwork.resultFont.reset();
    if (artwork.resultFont)
        artwork.resultFont->baseGlyphSize = 96;
    // Built by tools/generate_result_panels.py from sprite 231 and 435 frames 6 and 7.
    for (auto [panel, path] : { std::pair{ &artwork.resultPanelLost, "resources/game/images/result_panel_lost_id231.png" },
             std::pair{ &artwork.resultPanelWin, "resources/game/images/result_panel_win_id231.png" },
             std::pair{ &artwork.pactRefusedPanel, "resources/game/images/pact_refused_panel_id435.png" },
             std::pair{ &artwork.pactSignedPanel, "resources/game/images/pact_signed_panel_id435.png" },
             std::pair{ &artwork.capitulatePanel, "resources/game/images/capitulate_panel_id407.png" },
             std::pair{ &artwork.yourTurnPanels[0], "resources/game/images/your_turn_panel_1_id437.png" },
             std::pair{ &artwork.yourTurnPanels[1], "resources/game/images/your_turn_panel_2_id437.png" },
             std::pair{ &artwork.yourTurnPanels[2], "resources/game/images/your_turn_panel_3_id437.png" },
             std::pair{ &artwork.tutorialPanels[0], "resources/game/images/tutorial_panel_1_id192.png" },
             std::pair{ &artwork.tutorialPanels[1], "resources/game/images/tutorial_panel_2_id192.png" },
             std::pair{ &artwork.tutorialPanels[2], "resources/game/images/tutorial_panel_3_id192.png" },
             std::pair{ &artwork.pleaseWaitPanel, "resources/game/images/please_wait_panel_id207.png" },
             std::pair{ &artwork.mapGenerationScreen, "resources/game/sprites/sprite_80_hex_empire_splash_logo/frame_001.png" },
             std::pair{ &artwork.introBackdrop, "resources/game/shapes/black_square_texture_id250.png" },
             std::pair{ &artwork.introBanner, "resources/game/shapes/black_rectangle_banner_id8.png" },
             std::pair{ &artwork.introSponsorLogo, "resources/game/shapes/minijuegos_com_logo_wide_id260.png" },
             std::pair{ &artwork.introSpiral, "resources/game/images/intro_spiral_white_id21.png" },
             std::pair{ &artwork.introMetaSauce, "resources/game/images/intro_meta_sauce_id261.png" },
             std::pair{ &artwork.introMoreGames, "resources/game/shapes/more_strategy_games_wordmark_id263.png" },
             std::pair{ &artwork.introSkyline, "resources/game/shapes/magenta_gradient_overlay_id253.png" },
             std::pair{ &artwork.introVignette, "resources/game/shapes/gray_transparent_overlay_id251.png" },
             std::pair{ &artwork.introAppStore, "resources/game/sprites/sprite_29_app_store_badge/frame_001.png" } }) {
        *panel = std::make_unique<Texture>();
        (*panel)->loadFromFile(path, true, false);
        if ((*panel)->width <= 0 || (*panel)->height <= 0)
            panel->reset();
    }
    artwork.tutorialCapitalPin = std::make_unique<Texture>(
        "resources/game/images/capital_pin_red_id189.svg", true, false, true);
    if (artwork.tutorialCapitalPin->width <= 0 || artwork.tutorialCapitalPin->height <= 0)
        artwork.tutorialCapitalPin.reset();
    const auto loadFrames = [](auto& textures, const char* pattern) {
        for (size_t frame = 0; frame < textures.size(); ++frame) {
            char path[128];
            std::snprintf(path, sizeof(path), pattern, frame + 1);
            auto texture = std::make_unique<Texture>();
            texture->loadFromFile(path, true, false);
            if (texture->width > 0 && texture->height > 0) textures[frame] = std::move(texture);
        }
    };
    loadFrames(artwork.introCrest, "resources/game/sprites/sprite_78_hex_empire_crest_logo/frame_%03zu.png");
    loadFrames(artwork.introPhotos, "resources/game/sprites/sprite_249_archival_photo_panel/frame_%03zu.png");
    loadFrames(artwork.introText, "resources/game/images/intro_text_%03zu_id259.png");
    for (size_t index = 0; index < std::size(kIntroBlurredCrests) && index < artwork.introBlurredCrest.size(); ++index) {
        auto texture = std::make_unique<Texture>();
        texture->loadFromFile(std::string("resources/game/images/") + kIntroBlurredCrests[index].file, true, false);
        if (texture->width > 0 && texture->height > 0) artwork.introBlurredCrest[index] = std::move(texture);
    }
    for (size_t frame = 0; frame < artwork.loadingGauge.size(); ++frame) {
        char path[96];
        std::snprintf(path, sizeof(path),
            "resources/game/sprites/sprite_206_loading_progress_bar/frame_%03zu.png", frame + 1);
        auto gauge = std::make_unique<Texture>();
        gauge->loadFromFile(path, true, false);
        if (gauge->width > 0 && gauge->height > 0) artwork.loadingGauge[frame] = std::move(gauge);
    }
    artwork.mapLabelFont = std::make_unique<Font>("resources/game/fonts/199_Arno Pro Display.ttf");
    if (!artwork.mapLabelFont->font)
        artwork.mapLabelFont.reset();
    if (artwork.mapLabelFont)
        artwork.mapLabelFont->baseGlyphSize = 96;
    artwork.mapInputFont = std::make_unique<Font>("resources/game/fonts/174_FairfaxStation.ttf");
    if (!artwork.mapInputFont->font)
        artwork.mapInputFont.reset();
    if (artwork.mapInputFont)
        artwork.mapInputFont->baseGlyphSize = 96;
    artwork.audioToggleFont = std::make_unique<Font>("resources/game/fonts/285_Webdings.ttf");
    if (!artwork.audioToggleFont->font)
        artwork.audioToggleFont.reset();
    else {
        artwork.audioToggleFont->baseGlyphSize = 128;
        if (!artwork.audioToggleFont->loadGlyph(U'W') ||
            !artwork.audioToggleFont->loadGlyph(U'x'))
            artwork.audioToggleFont.reset();
    }
    artwork.helpIconFont = std::make_unique<Font>("resources/game/fonts/355_Webdings.ttf");
    if (!artwork.helpIconFont->font)
        artwork.helpIconFont.reset();
    else {
        artwork.helpIconFont->baseGlyphSize = 128;
        if (!artwork.helpIconFont->loadGlyph(U'i'))
            artwork.helpIconFont.reset();
    }

}

Texture* PickTexture(TextureSet& textures, int variant) {
    return textures[static_cast<size_t>(variant % static_cast<int>(textures.size()))].get();
}

void DrawTownMarker(glm::vec2 center, float scale, int capitalFor, bool armyPresent = false) {
    const bool capital = capitalFor >= 0;
    const float x = center.x + (armyPresent ? 18.0f : 0.0f) * scale;
    const float y = center.y + (armyPresent ? -4.0f : 0.0f) * scale;
    if (capital && static_cast<size_t>(capitalFor) < artwork.capitalDiscs.size() &&
        artwork.capitalDiscs[static_cast<size_t>(capitalFor)] && artwork.capitalStarOutline &&
        artwork.capitalStars[static_cast<size_t>(capitalFor)]) {
        const float vectorScale = (armyPresent ? 1.0f : 1.75f) * scale;
        const float discSize = 13.0f * vectorScale;
        const float starWidth = 10.6f * vectorScale;
        const float starHeight = 10.15f * vectorScale;
        const float outlineWidth = starWidth + 1.6f * scale;
        const float outlineHeight = starHeight + 1.6f * scale;
        if (artwork.capitalMarkerShadow) {
            // Keep the softened capital shadow separate from the crisp city marker.
            const float shadowSize = (armyPresent ? 20.0f : 35.0f) * scale;
            DrawRectangle(Rect(x - shadowSize * 0.5f, y - shadowSize * 0.5f,
                shadowSize, shadowSize), *artwork.capitalMarkerShadow);
        }
        DrawRectangle(Rect(x - discSize * 0.5f, y - discSize * 0.5f, discSize, discSize),
            *artwork.capitalDiscs[static_cast<size_t>(capitalFor)]);
        DrawRectangle(Rect(x - outlineWidth * 0.5f, y - outlineHeight * 0.5f,
            outlineWidth, outlineHeight), *artwork.capitalStarOutline);
        DrawRectangle(Rect(x - starWidth * 0.5f, y - starHeight * 0.5f, starWidth, starHeight),
            *artwork.capitalStars[static_cast<size_t>(capitalFor)]);
        return;
    }

    const size_t fallback = capital ? static_cast<size_t>(capitalFor + 1) : 0;
    if (fallback < artwork.townMarkerFallbacks.size() && artwork.townMarkerFallbacks[fallback]) {
        const float size = (armyPresent ? 20.0f : capital ? 35.0f : 20.0f) * scale;
        DrawRectangle(Rect(x - size * 0.5f, y - size * 0.5f, size, size),
            *artwork.townMarkerFallbacks[fallback]);
    }
}

const std::array<std::string, kFactionCount> kFactions = {
    "Redosia", "Violetnam", "Bluegaria", "Greenland"
};

const std::array<std::array<int, 2>, kFactionCount> kCapitals = {{
    {{1, 1}}, {{1, 9}}, {{18, 1}}, {{18, 9}}
}};

const std::vector<std::string> kCityNames = {
    R"city(Abu Dhabi)city",
    R"city(Abuja)city",
    R"city(Accra)city",
    R"city(Addis Ababa)city",
    R"city(Algiers)city",
    R"city(Amman)city",
    R"city(Amsterdam)city",
    R"city(Ankara)city",
    R"city(Antananarivo)city",
    R"city(Apia)city",
    R"city(Ashgabat)city",
    R"city(Asmara)city",
    R"city(Astana)city",
    R"city(Asunción)city",
    R"city(Athens)city",
    R"city(Baghdad)city",
    R"city(Baku)city",
    R"city(Bamako)city",
    R"city(Bangkok)city",
    R"city(Bangui)city",
    R"city(Banjul)city",
    R"city(Basseterre)city",
    R"city(Beijing)city",
    R"city(Beirut)city",
    R"city(Belgrade)city",
    R"city(Belmopan)city",
    R"city(Berlin)city",
    R"city(Bern)city",
    R"city(Bishkek)city",
    R"city(Bissau)city",
    R"city(Bogotá)city",
    R"city(Brasília)city",
    R"city(Bratislava)city",
    R"city(Brazzaville)city",
    R"city(Bridgetown)city",
    R"city(Brussels)city",
    R"city(Bucharest)city",
    R"city(Budapest)city",
    R"city(Buenos Aires)city",
    R"city(Bujumbura)city",
    R"city(Cairo)city",
    R"city(Canberra)city",
    R"city(Cape Town)city",
    R"city(Caracas)city",
    R"city(Castries)city",
    R"city(Chisinau)city",
    R"city(Conakry)city",
    R"city(Copenhagen)city",
    R"city(Cotonou)city",
    R"city(Dakar)city",
    R"city(Damascus)city",
    R"city(Dhaka)city",
    R"city(Dili)city",
    R"city(Djibouti)city",
    R"city(Dodoma)city",
    R"city(Doha)city",
    R"city(Dublin)city",
    R"city(Dushanbe)city",
    R"city(Delhi)city",
    R"city(Freetown)city",
    R"city(Funafuti)city",
    R"city(Gabarone)city",
    R"city(Georgetown)city",
    R"city(Guatemala City)city",
    R"city(Hague)city",
    R"city(Hanoi)city",
    R"city(Harare)city",
    R"city(Havana)city",
    R"city(Helsinki)city",
    R"city(Honiara)city",
    R"city(Hong Kong)city",
    R"city(Islamabad)city",
    R"city(Jakarta)city",
    R"city(Jerusalem)city",
    R"city(Kabul)city",
    R"city(Kampala)city",
    R"city(Kathmandu)city",
    R"city(Khartoum)city",
    R"city(Kyiv)city",
    R"city(Kigali)city",
    R"city(Kingston)city",
    R"city(Kingstown)city",
    R"city(Kinshasa)city",
    R"city(Kuala Lumpur)city",
    R"city(Kuwait City)city",
    R"city(La Paz)city",
    R"city(Liberville)city",
    R"city(Lilongwe)city",
    R"city(Lima)city",
    R"city(Lisbon)city",
    R"city(Ljubljana)city",
    R"city(Lobamba)city",
    R"city(Lomé)city",
    R"city(London)city",
    R"city(Luanda)city",
    R"city(Lusaka)city",
    R"city(Luxembourg)city",
    R"city(Madrid)city",
    R"city(Majuro)city",
    R"city(Malé)city",
    R"city(Managua)city",
    R"city(Manama)city",
    R"city(Manila)city",
    R"city(Maputo)city",
    R"city(Maseru)city",
    R"city(Mbabane)city",
    R"city(Melekeok)city",
    R"city(Mexico City)city",
    R"city(Minsk)city",
    R"city(Mogadishu)city",
    R"city(Monaco)city",
    R"city(Monrovia)city",
    R"city(Montevideo)city",
    R"city(Moroni)city",
    R"city(Moscow)city",
    R"city(Muscat)city",
    R"city(Nairobi)city",
    R"city(Nassau)city",
    R"city(Naypyidaw)city",
    R"city(N'Djamena)city",
    R"city(New Delhi)city",
    R"city(Niamey)city",
    R"city(Nicosia)city",
    R"city(Nouakchott)city",
    R"city(Nuku'alofa)city",
    R"city(Nuuk)city",
    R"city(Oslo)city",
    R"city(Ottawa)city",
    R"city(Ouagadougou)city",
    R"city(Palikir)city",
    R"city(Panama City)city",
    R"city(Paramaribo)city",
    R"city(Paris)city",
    R"city(Phnom Penh)city",
    R"city(Podgorica)city",
    R"city(Prague)city",
    R"city(Praia)city",
    R"city(Pretoria)city",
    R"city(Pyongyang)city",
    R"city(Quito)city",
    R"city(Rabat)city",
    R"city(Ramallah)city",
    R"city(Reykjavík)city",
    R"city(Riga)city",
    R"city(Riyadh)city",
    R"city(Rome)city",
    R"city(Roseau)city",
    R"city(San José)city",
    R"city(San Marino)city",
    R"city(San Salvador)city",
    R"city(Sanaá)city",
    R"city(Santiago)city",
    R"city(Santo Domingo)city",
    R"city(Sao Tomé)city",
    R"city(Sarajevo)city",
    R"city(Seoul)city",
    R"city(Singapore)city",
    R"city(Skopje)city",
    R"city(Sofia)city",
    R"city(South Tarawa)city",
    R"city(St. George's)city",
    R"city(St. John's)city",
    R"city(Stockholm)city",
    R"city(Sucre)city",
    R"city(Suva)city",
    R"city(Taipei)city",
    R"city(Tallinn)city",
    R"city(Tashkent)city",
    R"city(Tbilisi)city",
    R"city(Tegucigalpa)city",
    R"city(Teheran)city",
    R"city(Thimphu)city",
    R"city(Tirana)city",
    R"city(Tokyo)city",
    R"city(Tripoli)city",
    R"city(Tunis)city",
    R"city(Ulaanbaatar)city",
    R"city(Vaduz)city",
    R"city(Valletta)city",
    R"city(Victoria)city",
    R"city(Vienna)city",
    R"city(Vientiane)city",
    R"city(Vilnius)city",
    R"city(Warsaw)city",
    R"city(Washington)city",
    R"city(Wellington)city",
    R"city(Windhoek)city",
    R"city(Yamoussoukro)city",
    R"city(Yaoundé)city",
    R"city(Yerevan)city",
    R"city(Zagreb)city",
    R"city(Zielona Góra)city",
    R"city(Poznań)city",
    R"city(Wrocław)city",
    R"city(Gdańsk)city",
    R"city(Szczecin)city",
    R"city(Łódź)city",
    R"city(Białystok)city",
    R"city(Toruń)city",
    R"city(St. Petersburg)city",
    R"city(Turku)city",
    R"city(Örebro)city",
    R"city(Chengdu)city",
    R"city(Wuppertal)city",
    R"city(Frankfurt)city",
    R"city(Düsseldorf)city",
    R"city(Essen)city",
    R"city(Duisburg)city",
    R"city(Magdeburg)city",
    R"city(Bonn)city",
    R"city(Brno)city",
    R"city(Tours)city",
    R"city(Bordeaux)city",
    R"city(Nice)city",
    R"city(Lyon)city",
    R"city(Stara Zagora)city",
    R"city(Milan)city",
    R"city(Bologna)city",
    R"city(Sydney)city",
    R"city(Venice)city",
    R"city(New York)city",
    R"city(Barcelona)city",
    R"city(Zaragoza)city",
    R"city(Valencia)city",
    R"city(Seville)city",
    R"city(Graz)city",
    R"city(Munich)city",
    R"city(Birmingham)city",
    R"city(Naples)city",
    R"city(Cologne)city",
    R"city(Turin)city",
    R"city(Marseille)city",
    R"city(Leeds)city",
    R"city(Kraków)city",
    R"city(Palermo)city",
    R"city(Genoa)city",
    R"city(Stuttgart)city",
    R"city(Dortmund)city",
    R"city(Rotterdam)city",
    R"city(Glasgow)city",
    R"city(Málaga)city",
    R"city(Bremen)city",
    R"city(Sheffield)city",
    R"city(Antwerp)city",
    R"city(Plovdiv)city",
    R"city(Thessaloniki)city",
    R"city(Kaunas)city",
    R"city(Lublin)city",
    R"city(Varna)city",
    R"city(Ostrava)city",
    R"city(Iaşi)city",
    R"city(Katowice)city",
    R"city(Cluj-Napoca)city",
    R"city(Timişoara)city",
    R"city(Constanţa)city",
    R"city(Pskov)city",
    R"city(Vitebsk)city",
    R"city(Arkhangelsk)city",
    R"city(Novosibirsk)city",
    R"city(Samara)city",
    R"city(Omsk)city",
    R"city(Chelyabinsk)city",
    R"city(Ufa)city",
    R"city(Volgograd)city",
    R"city(Perm)city",
    R"city(Kharkiv)city",
    R"city(Odessa)city",
    R"city(Donetsk)city",
    R"city(Dnipropetrovsk)city",
    R"city(Los Angeles)city",
    R"city(Chicago)city",
    R"city(Houston)city",
    R"city(Phoenix)city",
    R"city(Philadelphia)city",
    R"city(Dallas)city",
    R"city(Detroit)city",
    R"city(Indianapolis)city",
    R"city(San Francisco)city",
    R"city(Atlanta)city",
    R"city(Austin)city",
    R"city(Vermont)city",
    R"city(Toronto)city",
    R"city(Montreal)city",
    R"city(Vancouver)city",
    R"city(Gdynia)city",
    R"city(Edmonton)city"
};

glm::vec4 Color(int r, int g, int b, int a = 255) {
    return glm::vec4(r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f);
}

glm::vec4 FactionColor(int faction) {
    switch (faction) {
    case 0: return Color(222, 48, 40);
    case 1: return Color(131, 80, 171);
    case 2: return Color(44, 119, 166);
    default: return Color(56, 133, 82);
    }
}

bool CanSelectPlayerArmy(int index) {
    if (index < 0 || index >= kTileCount) return false;
    const Tile& tile = game.tiles[index];
    const int reservedMoves = (game.movingArmyFrom >= 0 ? 1 : 0) + (game.queuedArmyFrom >= 0 ? 1 : 0);
    return game.screen == Screen::Playing && game.currentPlayer == game.human &&
        !game.endTurnRequested && game.movesLeft > reservedMoves && tile.army.present &&
        tile.army.owner == game.human && !tile.army.moved &&
        index != game.movingArmyFrom && index != game.movingArmyTarget &&
        index != game.queuedArmyFrom && index != game.queuedArmyTarget;
}

bool IsMovableArmy(const Tile& tile) {
    return game.selectedTile < 0 && CanSelectPlayerArmy(static_cast<int>(&tile - game.tiles.data()));
}

glm::vec4 FactionTerritoryColor(int faction) {
    // These are the original ActionScript hw_parties_colors values.
    switch (faction) {
    case 0: return Color(255, 0, 0);       // 0xFF0000
    case 1: return Color(255, 0, 255);     // 0xFF00FF
    case 2: return Color(0, 187, 255);     // 0x00BBFF
    default: return Color(0, 255, 0);     // 0x00FF00
    }
}

float ArmyBlinkOpacity(const Army& army) {
    if (army.blinkElapsed < 0.0f) return 1.0f;
    const int frame = static_cast<int>(army.blinkElapsed * 12.0f);
    if (frame >= kArmyBlinkFrames) return 1.0f;
    // The toggle runs before the frame renders, so frames 0-1 are at 50.
    return (frame / kArmyBlinkPeriod) % 2 == 0 ? 0.5f : 1.0f;
}

void UpdateArmyBlinks(float deltaTime) {
    for (Tile& tile : game.tiles) {
        Army& army = tile.army;
        if (!army.present || army.blinkElapsed < 0.0f) continue;
        army.blinkElapsed += std::max(0.0f, deltaTime);
        if (army.blinkElapsed * 12.0f >= kArmyBlinkFrames) army.blinkElapsed = -1.0f;
    }
}

void DrawArmyMarker(int tileIndex, const Tile& tile, glm::vec2 center, float scale) {
    if (!tile.army.present) return;
    // _alpha applies to the whole sprite 70 clip, including its info badge.
    const float clipOpacity = ArmyBlinkOpacity(tile.army);

    EnsureArmyUnitTexturesForScale(scale);
    const size_t faction = static_cast<size_t>(std::clamp(tile.army.owner, 0, kFactionCount - 1));
    const int armyCount = std::clamp(tile.army.manpower, 0, 99);
    const size_t unitVariant = static_cast<size_t>(std::clamp(armyCount / 40, 0, 2));
    Texture* unit = tile.land
        ? artwork.armyLandUnits[unitVariant][faction].get()
        : artwork.armyWaterUnits[unitVariant][faction].get();

    if (unit) {
        // sprite_70 places sprite_69 at (+0.45, +3.05); the filtered texture
        // adds a 7 px transparent margin around the original 34.45x27.2 frame.
        const float filterPadding = 7.0f;
        const float iconWidth = (kArmyUnitCanvasWidth + filterPadding * 2.0f) * scale;
        const float iconHeight = (kArmyUnitCanvasHeight + filterPadding * 2.0f) * scale;
        const float iconX = center.x + (0.45f - 17.4f - filterPadding) * scale;
        const float iconY = center.y + (3.05f - 17.15f - filterPadding) * scale;
        GLint previousBlendSrcRgb = GL_SRC_ALPHA;
        GLint previousBlendDstRgb = GL_ONE_MINUS_SRC_ALPHA;
        GLint previousBlendSrcAlpha = GL_ONE;
        GLint previousBlendDstAlpha = GL_ONE_MINUS_SRC_ALPHA;
        const GLboolean previousBlendEnabled = glIsEnabled(GL_BLEND);
        glGetIntegerv(GL_BLEND_SRC_RGB, &previousBlendSrcRgb);
        glGetIntegerv(GL_BLEND_DST_RGB, &previousBlendDstRgb);
        glGetIntegerv(GL_BLEND_SRC_ALPHA, &previousBlendSrcAlpha);
        glGetIntegerv(GL_BLEND_DST_ALPHA, &previousBlendDstAlpha);
        glEnable(GL_BLEND);
        glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA,
            GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        // Premultiplied texels: scale every channel to fade the icon.
        DrawRectangle(Rect(iconX, iconY, iconWidth, iconHeight),
            glm::vec4(clipOpacity), *unit);
        glBlendFuncSeparate(previousBlendSrcRgb, previousBlendDstRgb,
            previousBlendSrcAlpha, previousBlendDstAlpha);
        if (previousBlendEnabled) glEnable(GL_BLEND); else glDisable(GL_BLEND);
    }

    // sprite_70's info child starts at alpha 0 and the field-hover clip eases
    // it toward alpha 100 only while the pointer is within 100 SWF units.
    const float infoOpacity = tileIndex >= 0 && tileIndex < kTileCount
        ? std::clamp(game.armyInfoAlpha[static_cast<size_t>(tileIndex)], 0.0f, 1.0f) : 0.0f;
    const float counterOpacity = infoOpacity * clipOpacity;
    if (counterOpacity > 0.005f && artwork.armyManpowerMoraleBadge) {
        // On hover sprite_51 uses frame 2: shape 47's translucent red/white
        // split badge with manpower in white and morale in red.
        const float badgeWidth = 30.05f * scale;
        const float badgeHeight = 29.95f * scale;
        const float badgeCenterY = center.y + 0.45f * scale;
        DrawRectangle(Rect(center.x - badgeWidth * 0.5f,
            badgeCenterY - badgeHeight * 0.5f, badgeWidth, badgeHeight),
            glm::vec4(1.0f, 1.0f, 1.0f, counterOpacity), *artwork.armyManpowerMoraleBadge);

        // Font 48 carries the SWF's bold glyphs; avoid adding SDF emboldening.
        // The SWF uses embedded ascent metrics for the first-line baselines,
        // not the visible-ink center. The fields' yMin=-40 twips cancels the
        // EditText 40-twip inset. Font-48 ascent is 18260/20480 em; fields
        // 50/49 use d=1.020981, while sprite_51 adds d=1.004288.
        // Their field centers remain at (-2.869,-6.563) and (+2.881,+7.447)
        // px in sprite_70, and their baseline positions below are composed
        // from the serialized field translations and font ascent.
        Font* previousBadgeFont = extras::defaultTextRenderer
            ? extras::defaultTextRenderer->font : nullptr;
        const int previousBadgeWeight = extras::defaultTextRenderer
            ? extras::defaultTextRenderer->fontWeight : 400;
        const float previousBadgeHorizontalScale = extras::defaultTextRenderer
            ? extras::defaultTextRenderer->horizontalScale : 1.0f;
        const int badgeTextAlpha = static_cast<int>(std::lround(255.0f * counterOpacity));
        if (artwork.armyBadgeFont && extras::defaultTextRenderer)
            extras::defaultTextRenderer->setFont(*artwork.armyBadgeFont);
        if (extras::defaultTextRenderer)
            extras::defaultTextRenderer->setFontWeight(400);
        constexpr float badgeFontAscentEm = 18260.0f / 20480.0f;
        constexpr float badgeFieldScaleX = 1.01959228515625f;
        constexpr float badgeFieldScaleY = 1.0209808349609375f;
        constexpr float badgeInfoScaleY = 1.0042877197265625f;
        const float badgeTextHeight = 12.0f * badgeFieldScaleY * badgeInfoScaleY * scale;
        const float badgeTextHorizontalScale = badgeFieldScaleX / (badgeFieldScaleY * badgeInfoScaleY);
        if (extras::defaultTextRenderer)
            extras::defaultTextRenderer->setHorizontalScale(badgeTextHorizontalScale);
        const float badgeFontAscentPx = 12.0f * badgeFontAscentEm;
        const float countBaselineY = badgeCenterY +
            (-278.0f / 20.0f + badgeFieldScaleY * badgeFontAscentPx) * badgeInfoScaleY * scale;
        const float moraleBaselineY = badgeCenterY +
            (1.0f / 20.0f + badgeFieldScaleY * badgeFontAscentPx) * badgeInfoScaleY * scale;
        DrawText(std::to_string(tile.army.manpower),
            glm::vec2(center.x - 2.869f * scale, countBaselineY), badgeTextHeight,
            Color(255, 255, 255, badgeTextAlpha), TextAlignmentX::CENTER, TextAlignmentY::BASELINE);
        DrawText(std::to_string(tile.army.morale),
            glm::vec2(center.x + 2.881f * scale, moraleBaselineY), badgeTextHeight,
            Color(255, 0, 0, badgeTextAlpha), TextAlignmentX::CENTER, TextAlignmentY::BASELINE);
        if (extras::defaultTextRenderer)
            extras::defaultTextRenderer->setHorizontalScale(previousBadgeHorizontalScale);
        if (previousBadgeFont && extras::defaultTextRenderer)
            extras::defaultTextRenderer->setFont(*previousBadgeFont);
        if (extras::defaultTextRenderer)
            extras::defaultTextRenderer->setFontWeight(previousBadgeWeight);
    }
}

glm::vec4 FactionLight(int faction) {
    return glm::mix(Color(215, 204, 173), FactionColor(faction), 0.48f);
}

int Index(int x, int y) { return y * kMapWidth + x; }
int TileX(int index) { return index % kMapWidth; }
int TileY(int index) { return index / kMapWidth; }

bool IsLandForShoreline(const Tile& tile) {
    return tile.land || tile.estate == Estate::Port;
}

int NeighbourInDirection(int index, int direction) {
    if (direction < 0 || direction >= 6) return -1;
    const int x = TileX(index);
    const int y = TileY(index);
    const int offsetsEven[6][2] = { {1,0}, {0,1}, {-1,0}, {-1,-1}, {0,-1}, {1,-1} };
    const int offsetsOdd[6][2] = { {1,1}, {0,1}, {-1,1}, {-1,0}, {0,-1}, {1,0} };
    const auto& offsets = (x % 2 == 0) ? offsetsEven : offsetsOdd;
    const int nx = x + offsets[direction][0];
    const int ny = y + offsets[direction][1];
    if (nx < 0 || nx >= kMapWidth || ny < 0 || ny >= kMapHeight)
        return -1;
    return Index(nx, ny);
}

Tile& At(int x, int y) { return game.tiles[Index(x, y)]; }

std::string MapSeedText() {
    return game.blankMapNumber ? std::string() : std::to_string(game.mapNumber);
}

// News titles are built as "" + town_name, so a missing name reads "undefined".
std::string NewsTownName(const Tile& tile) {
    return tile.townName.empty() ? std::string("undefined") : tile.townName;
}

int RandomInt(int minValue, int maxValue) {
    std::uniform_int_distribution<int> distribution(minValue, maxValue);
    return distribution(game.random);
}

int LegacyMapRandom(std::int64_t& seed, int range) {
    seed = (seed * 9301 + 49297) % 233280;
    return static_cast<int>(seed / 233280.0 * range);
}

struct SeaArtSelection {
    int variant = 0;
    bool flipHorizontal = false;
    bool flipVertical = false;
    int rotation = 0;
};

SeaArtSelection SelectSeaArtLikeSource(std::int64_t& seed) {
    // The ActionScript selects m_(rand(6) + 1), then consumes horizontal,
    // vertical, and 180-degree rotation draws in that order. Texture arrays
    // are zero-based here, so convert only the selected asset number.
    const int sourceVariant = LegacyMapRandom(seed, 6) + 1;
    const bool flipHorizontal = LegacyMapRandom(seed, 2) != 0;
    const bool flipVertical = LegacyMapRandom(seed, 2) != 0;
    const int rotation = LegacyMapRandom(seed, 2) * 180;
    return {
        sourceVariant - 1,
        flipHorizontal,
        flipVertical,
        rotation
    };
}

std::vector<int> Neighbours(int index) {
    std::vector<int> result;
    result.reserve(6);
    for (int direction = 0; direction < 6; ++direction)
        if (const int neighbour = NeighbourInDirection(index, direction); neighbour >= 0)
            result.push_back(neighbour);
    return result;
}

bool IsAlive(int faction) {
    const int cap = Index(kCapitals[faction][0], kCapitals[faction][1]);
    return game.tiles[cap].owner == faction;
}

int ArmyCount(int faction) {
    int count = 0;
    for (const Tile& tile : game.tiles)
        if (tile.army.present && tile.army.owner == faction) ++count;
    return count;
}

// checkPartyState's hw_parties_provinces_cp: rival capitals the faction holds
// whose own kingdom has no armies left.
bool IsProvinceOf(const Tile& tile, int faction) {
    return tile.capitalFor >= 0 && tile.capitalFor != faction && tile.owner == faction &&
        ArmyCount(tile.capitalFor) == 0;
}

int ProvinceCount(int faction) {
    int provinces = 0;
    for (const Tile& tile : game.tiles)
        if (IsProvinceOf(tile, faction)) ++provinces;
    return provinces;
}

// getMovableArmies: the faction's armies that have not moved this turn.
int MovableArmyCount(int faction) {
    int count = 0;
    for (const Tile& tile : game.tiles)
        if (tile.army.present && tile.army.owner == faction && !tile.army.moved) ++count;
    return count;
}

int ManpowerTotal(int faction) {
    int count = 0;
    for (const Tile& tile : game.tiles)
        if (tile.army.present && tile.army.owner == faction) count += tile.army.manpower;
    return count;
}

int FactionRecruitMorale(int faction) {
    int armyCount = 0;
    int moraleTotal = 0;
    const int moraleFloor = ManpowerTotal(faction) / 50;
    for (Tile& tile : game.tiles) {
        Army& army = tile.army;
        if (!army.present || army.owner != faction) continue;
        // updateBoard in the SWF raises armies below this faction-wide floor
        // before calculating its average morale for the next recruitment.
        if (army.morale < moraleFloor)
            army.morale = std::min(moraleFloor, army.manpower);
        moraleTotal += army.morale;
        ++armyCount;
    }
    return armyCount > 0 ? moraleTotal / armyCount : 10;
}

int LandTotal(int faction) {
    int count = 0;
    for (const Tile& tile : game.tiles)
        if (tile.land && tile.owner == faction && tile.estate == Estate::None) ++count;
    return count;
}

int TownTotal(int faction) {
    int count = 0;
    for (const Tile& tile : game.tiles)
        if (tile.owner == faction && tile.estate == Estate::Town) ++count;
    return count;
}

int PortTotal(int faction) {
    int count = 0;
    for (const Tile& tile : game.tiles)
        if (tile.owner == faction && tile.estate == Estate::Port) ++count;
    return count;
}

void AddNews(const std::string& message, NewspaperArticle article = NewspaperArticle::Peace, int priority = 0) {
    if (message.empty() || (game.collectingMoveNews && priority < game.newsPriority)) return;
    if (game.collectingMoveNews) game.newsPriority = priority;
    ++game.newsSerial;
    game.newspaperArticle = article;
    game.news.insert(game.news.begin(), message);
    if (game.news.size() > 4) game.news.pop_back();
}

void AddMorale(Army& army, int amount) {
    if (!army.present) return;
    army.morale = std::clamp(army.morale + amount, 0, army.manpower);
}

void AddMoraleToFaction(int faction, int amount) {
    if (faction < 0 || amount == 0) return;
    for (Tile& tile : game.tiles)
        if (tile.army.present && tile.army.owner == faction) AddMorale(tile.army, amount);
}

void AddArmy(Tile& tile, int faction, int manpower, int morale) {
    if (manpower <= 0) return;
    if (!tile.army.present) {
        tile.army.present = true;
        tile.army.owner = faction;
        tile.army.manpower = std::min(99, manpower);
        tile.army.morale = std::clamp(morale, 0, tile.army.manpower);
        tile.army.moved = false;
        // updateArmy attaches a fresh sprite 70 here, which blinks.
        tile.army.blinkElapsed = 0.0f;
        return;
    }
    if (tile.army.owner != faction) return;
    const int previous = tile.army.manpower;
    const int added = std::min(manpower, 99 - previous);
    if (added <= 0) return;
    const int combined = previous + added;
    tile.army.morale = std::clamp((previous * tile.army.morale + added * morale) / combined, 0, combined);
    tile.army.manpower = combined;
}

void SpawnRecruits(int faction) {
    if (!IsAlive(faction)) return;
    const int townCount = std::max(1, TownTotal(faction));
    const int reinforcementShare = (LandTotal(faction) + PortTotal(faction) * 5) / townCount;
    const int factionMorale = FactionRecruitMorale(faction);
    // SWF unitsSpawn creates the small capital force first. Its morale is
    // capped to its five manpower, then the town recruitment joins at that
    // existing morale, which is why an initial 16-man capital army has morale 5.
    for (int i = 0; i < kTileCount; ++i) {
        Tile& tile = game.tiles[i];
        if (tile.owner != faction || tile.estate != Estate::Town || tile.capitalFor < 0) continue;
        const int morale = tile.army.present ? tile.army.morale : factionMorale;
        AddArmy(tile, faction, 5, morale);
    }
    for (int i = 0; i < kTileCount; ++i) {
        Tile& tile = game.tiles[i];
        if (tile.owner != faction || tile.estate != Estate::Town) continue;
        const int morale = tile.army.present ? tile.army.morale : factionMorale;
        AddArmy(tile, faction, 5 + reinforcementShare, morale);
    }
}

void ClaimOne(int index, int faction, Army* victoriousArmy, bool startup) {
    Tile& tile = game.tiles[index];
    if (!tile.land || tile.owner == faction) return;

    const int previousOwner = tile.owner;
    const int newsPriority = tile.capitalFor >= 0 ? 3 : tile.estate == Estate::Town ? 2 : 1;
    if (previousOwner >= 0 && previousOwner != faction) {
        int moraleLoss = 0;
        if (tile.capitalFor >= 0) moraleLoss = 30;
        else if (tile.estate == Estate::Town) moraleLoss = 10;
        else if (tile.estate == Estate::Port) moraleLoss = 5;
        AddMoraleToFaction(previousOwner, -moraleLoss);
    }

    tile.owner = faction;
    if (!startup && victoriousArmy) {
        int armyGain = 0;
        int factionGain = 0;
        if (tile.capitalFor >= 0) {
            const bool sovereignCapital = previousOwner == tile.capitalFor;
            factionGain = sovereignCapital ? 50 : 30;
            armyGain = sovereignCapital ? 30 : 20;
            if (faction == game.human && !sovereignCapital)
                AddNews(NewsTownName(tile) + " captured!", NewspaperArticle::Captured, newsPriority);
            else if (previousOwner == game.human)
                AddNews(NewsTownName(tile) + " lost.", NewspaperArticle::Lost, newsPriority);
        } else if (tile.estate == Estate::Town && tile.capitalFor < 0) {
            factionGain = 10;
            armyGain = 10;
            if (faction == game.human) {
                if (previousOwner < 0)
                    AddNews(NewsTownName(tile) + " annexed!", NewspaperArticle::Annexed, newsPriority);
                else
                    AddNews(NewsTownName(tile) + " captured!", NewspaperArticle::Captured, newsPriority);
            } else if (previousOwner == game.human) {
                AddNews(NewsTownName(tile) + " lost.", NewspaperArticle::Lost, newsPriority);
            }
        } else if (tile.estate == Estate::Port) {
            factionGain = 5;
            armyGain = 5;
            if (faction == game.human) {
                if (previousOwner < 0)
                    AddNews(NewsTownName(tile) + " annexed!", NewspaperArticle::Annexed, newsPriority);
                else
                    AddNews(NewsTownName(tile) + " captured!", NewspaperArticle::Captured, newsPriority);
            } else if (previousOwner == game.human) {
                AddNews(NewsTownName(tile) + " lost.", NewspaperArticle::Lost, newsPriority);
            }
        } else {
            factionGain = 1;
        }
        AddMoraleToFaction(faction, factionGain);
        AddMorale(*victoriousArmy, armyGain);
    }
}

void AnnexLand(int index, int faction, Army* victoriousArmy, bool startup) {
    Tile& tile = game.tiles[index];
    if (!tile.land) return;
    ClaimOne(index, faction, victoriousArmy, startup);
    for (int neighbour : Neighbours(index)) {
        Tile& next = game.tiles[neighbour];
        // annexLand leaves the human's and the pact partner's fields to each other.
        const bool pactProtected = game.pactPartner >= 0 &&
            ((faction == game.human && next.owner == game.pactPartner) ||
             (faction == game.pactPartner && next.owner == game.human));
        if (next.land && next.estate == Estate::None && !next.army.present && !pactProtected)
            ClaimOne(neighbour, faction, victoriousArmy, startup);
    }
}

// fireExplosion plus sprite 167 frame 4: s_explosion_n plays only while fewer
// than two explosions that played it are running (expl_sound_cnt), and the
// field gains 20 doom and its neighbours 10.
void StartExplosion(int tile, const Army& exploding, bool ghostOnTop = true) {
    const auto soundingExplosions = std::count_if(game.explosions.begin(), game.explosions.end(),
        [](const BattleExplosion& explosion) { return explosion.soundPlayed; });
    BattleExplosion explosion{ tile, 0.0f };
    explosion.ghost = exploding;
    explosion.ghost.blinkElapsed = -1.0f;
    explosion.ghostOnTop = ghostOnTop;
    if (soundingExplosions < 2 && gameAudio.explosion) {
        gameAudio.explosion->play();
        explosion.soundPlayed = true;
    }
    game.explosions.push_back(explosion);
    game.fieldDoom[static_cast<size_t>(tile)] += 20.0f;
    for (int neighbour : Neighbours(tile))
        game.fieldDoom[static_cast<size_t>(neighbour)] += 10.0f;
}

// annexLand: when an empire's own capital falls, each province it holds
// revolts. Its garrison explodes, the original kingdom returns there with a
// 99/99 army, and that kingdom re-annexes the capital and its neighbours.
void LiberateProvinces(int empire) {
    std::vector<int> provinces;
    for (int index = 0; index < kTileCount; ++index)
        if (IsProvinceOf(game.tiles[static_cast<size_t>(index)], empire)) provinces.push_back(index);
    for (int index : provinces) {
        Tile& tile = game.tiles[static_cast<size_t>(index)];
        const int kingdom = tile.capitalFor;
        if (tile.army.present) {
            StartExplosion(index, tile.army);
            tile.army = Army{};
        }
        AddArmy(tile, kingdom, 99, 99);
        AnnexLand(index, kingdom, nullptr, true);
    }
}

void EliminateFaction(int defeated, int victor) {
    if (defeated < 0 || victor < 0 || defeated == victor) return;
    // updateBoard hands a fallen kingdom's fields to whoever holds its capital
    // and sets every one of its armies exploding.
    for (int index = 0; index < kTileCount; ++index) {
        Tile& tile = game.tiles[static_cast<size_t>(index)];
        if (tile.owner == defeated) tile.owner = victor;
        if (tile.army.present && tile.army.owner == defeated) {
            StartExplosion(index, tile.army);
            tile.army = Army{};
        }
    }
    if (victor == game.human)
        AddNews(kFactions[defeated] + " on knees!", NewspaperArticle::ProvinceConquered, 3);
}

// The SWF attaches sprite 231 over the frozen board for both endings.
void ShowResultScreen(bool playerWon) {
    game.playerWon = playerWon;
    game.screen = Screen::Result;
    game.resultElapsed = 0.0f;
    game.selectedTile = -1;
    game.hoveredTile = -1;
    game.moveTargets.clear();
    game.endTurnRequested = false;
    // Sprite 61 hints belong to the army clips on the board, which stay on
    // stage under sprite 231, so they are left as they were.
}

bool CheckOutcome() {
    if (game.screen != Screen::Playing) return true;
    // moraleLost: losing the human's own capital ends the game at once.
    if (!IsAlive(game.human)) {
        ShowResultScreen(false);
        return true;
    }
    // moraleEarned set board.win for this capture; there is no other win path.
    if (game.humanConquered) {
        ShowResultScreen(true);
        return true;
    }
    return false;
}

int FindTileAt(glm::vec2 point, float scale, float left, float top) {
    const float halfW = 25.0f * scale;
    const float halfH = 20.0f * scale;
    const float slope = halfW * 0.5f / halfH;
    for (int x = 0; x < kMapWidth; ++x) {
        for (int y = 0; y < kMapHeight; ++y) {
            const float cx = left + halfW + x * 37.5f * scale;
            const float cy = top + halfH + y * 40.0f * scale + (x % 2 ? 20.0f * scale : 0.0f);
            const float dx = std::abs(point.x - cx);
            const float dy = std::abs(point.y - cy);
            if (dy <= halfH && dx <= halfW - slope * dy)
                return Index(x, y);
        }
    }
    return -1;
}

std::vector<int> PossibleMoves(int from, bool preserveDuplicates = false) {
    const Tile& start = game.tiles[from];
    std::vector<int> result;
    auto canEnter = [&](int target) {
        if (target == from) return false;
        const Tile& next = game.tiles[target];
        if (!next.army.present) return true;
        if (next.army.owner != start.army.owner) return true;
        return next.land && next.army.manpower < 99;
    };
    auto add = [&](int target) {
        if (canEnter(target) && (preserveDuplicates ||
                std::find(result.begin(), result.end(), target) == result.end()))
            result.push_back(target);
    };

    const auto firstStep = Neighbours(from);
    if (start.estate == Estate::Port) {
        for (int middle : firstStep) {
            add(middle);
            const Tile& mid = game.tiles[middle];
            if (mid.army.present) continue;
            for (int target : Neighbours(middle)) {
                const Tile& next = game.tiles[target];
                if (mid.land && mid.estate == Estate::None && next.land) add(target);
                else if (!mid.land && !next.land) add(target);
            }
        }
    } else if (!start.land) {
        for (int middle : firstStep) {
            add(middle);
            const Tile& mid = game.tiles[middle];
            if (mid.land || mid.army.present) continue;
            for (int target : Neighbours(middle)) add(target);
        }
    } else {
        for (int middle : firstStep) {
            const Tile& mid = game.tiles[middle];
            if (!mid.land) continue;
            add(middle);
            if (mid.estate != Estate::None || mid.army.present) continue;
            for (int target : Neighbours(middle))
                if (game.tiles[target].land) add(target);
        }
    }
    return result;
}

template <typename T, typename Compare>
void FlashArraySort(std::vector<T>& values, Compare compare) {
    if (values.size() < 2) return;

    // ActionScript's AVM1 Array.sort uses a left-pivot quicksort. Preserve its
    // deterministic tie permutations because the AI comparator often returns 0.
    std::vector<std::pair<size_t, size_t>> ranges;
    ranges.emplace_back(0, values.size() - 1);
    while (!ranges.empty()) {
        const auto [low, high] = ranges.back();
        ranges.pop_back();
        if (low >= high) continue;

        const T pivot = values[low];
        size_t left = low + 1;
        size_t right = high;
        while (true) {
            while (left < right && compare(pivot, values[left]) > 0) ++left;
            while (right > low && compare(pivot, values[right]) <= 0) --right;
            if (left >= right) break;
            std::swap(values[left], values[right]);
        }
        std::swap(values[low], values[right]);

        ranges.emplace_back(right + 1, high);
        if (right > 0) ranges.emplace_back(low, right - 1);
    }
}

int HexDistance(int first, int second) {
    const int ax = TileX(first);
    const int bx = TileX(second);
    const int ar = TileY(first) - (ax - ax % 2) / 2;
    const int br = TileY(second) - (bx - bx % 2) / 2;
    const int aq = ax;
    const int bq = bx;
    const int as = -aq - ar;
    const int bs = -bq - br;
    return std::max({ std::abs(aq - bq), std::abs(ar - br), std::abs(as - bs) });
}

struct AITileMetrics {
    std::array<int, kFactionCount> capitalPathLengths{};
    std::array<bool, kFactionCount> nearCapital{};
    std::array<int, 18> nearbyTiles{};
    int nearbyTileCount = 0;
    bool nearTown = false;
};

struct AIMapMetrics {
    std::array<AITileMetrics, kTileCount> tiles{};
    std::array<int, kFactionCount> capitalTiles{};
};

struct AIFactionTotals {
    std::array<int, kFactionCount> power{};
};

struct AIMoveChoice {
    int from = -1;
    int target = -1;
    int power = 0;
    float profitability = -std::numeric_limits<float>::infinity();
    bool waitForSupport = false;
};

int CompareAIMoves(const AIMoveChoice& first, const AIMoveChoice& second) {
    if (first.profitability > second.profitability) return -1;
    if (first.profitability < second.profitability) return 1;
    return 0;
}

int CompareAIArmies(const AIMoveChoice& first, const AIMoveChoice& second) {
    if (first.profitability > second.profitability) return -1;
    if (first.profitability < second.profitability) return 1;
    if (first.power > second.power) return -1;
    if (first.power < second.power) return 1;
    return 0;
}

float AIDistance(int first, int second);

int AIPathLength(int start, int end) {
    if (start < 0 || start >= kTileCount || end < 0 || end >= kTileCount) return 0;

    struct PathNode {
        int field = -1;
        int parentField = -1;
        int travelCost = 0;
        float distanceCost = 0.0f;
    };
    std::array<int, kTileCount> openNodeIndex;
    std::array<int, kTileCount> closedNodeIndex;
    openNodeIndex.fill(-1);
    closedNodeIndex.fill(-1);
    std::vector<PathNode> nodes;
    std::vector<int> open;
    std::vector<int> closed;
    nodes.push_back({ start, -1, 0, 0.0f });
    open.push_back(0);
    openNodeIndex[start] = 0;
    const bool avoidWater = game.tiles[start].land;

    while ((closed.empty() || nodes[static_cast<size_t>(closed.back())].field != end) && !open.empty()) {
        const int currentNodeIndex = open.front();
        open.erase(open.begin());
        const PathNode current = nodes[static_cast<size_t>(currentNodeIndex)];
        for (int direction = 0; direction < 6; ++direction) {
            const int neighbour = NeighbourInDirection(current.field, direction);
            if (neighbour < 0) continue;
            const Tile& from = game.tiles[current.field];
            const Tile& to = game.tiles[neighbour];
            bool canWalk = true;
            if (canWalk && avoidWater) {
                canWalk = (!from.land && !to.land) || (from.land && to.land) ||
                    (!from.land && to.land) || (!to.land && from.estate == Estate::Port);
            }
            if (!canWalk && neighbour != end) continue;

            const float distance = AIDistance(neighbour, end);
            const PathNode next{ neighbour, current.field, current.travelCost + 5, 5.0f + distance };
            if (closedNodeIndex[neighbour] >= 0) {
                const int closedPosition = closedNodeIndex[neighbour];
                const int oldNodeIndex = closed[static_cast<size_t>(closedPosition)];
                if (nodes[static_cast<size_t>(oldNodeIndex)].travelCost > next.travelCost) {
                    nodes.push_back(next);
                    const int replacementIndex = static_cast<int>(nodes.size()) - 1;
                    closed[static_cast<size_t>(closedPosition)] = replacementIndex;
                }
            } else if (openNodeIndex[neighbour] < 0) {
                nodes.push_back(next);
                const int nextNodeIndex = static_cast<int>(nodes.size()) - 1;
                openNodeIndex[neighbour] = nextNodeIndex;
                open.push_back(nextNodeIndex);
            }
        }

        closedNodeIndex[current.field] = static_cast<int>(closed.size());
        closed.push_back(currentNodeIndex);
        if (!open.empty()) {
            size_t best = 0;
            for (size_t i = 1; i < open.size(); ++i) {
                if (nodes[static_cast<size_t>(open[i])].distanceCost <
                    nodes[static_cast<size_t>(open[best])].distanceCost) {
                    best = i;
                }
            }
            std::swap(open.front(), open[best]);
        }
    }

    if (open.empty() || closed.empty() || nodes[static_cast<size_t>(closed.back())].field != end) return 0;
    int pathLength = 0;
    int field = end;
    while (field != start) {
        ++pathLength;
        const int nodeIndex = closedNodeIndex[field];
        if (nodeIndex < 0) return 0;
        field = nodes[static_cast<size_t>(closed[static_cast<size_t>(nodeIndex)])].parentField;
        if (field < 0) return 0;
    }
    return pathLength + 1;
}

AIMapMetrics BuildAIMapMetrics() {
    AIMapMetrics metrics;
    metrics.capitalTiles.fill(-1);
    for (int i = 0; i < kTileCount; ++i) {
        const int faction = game.tiles[i].capitalFor;
        if (faction >= 0 && faction < kFactionCount) metrics.capitalTiles[faction] = i;
    }

    for (int field = 0; field < kTileCount; ++field) {
        AITileMetrics& fieldMetrics = metrics.tiles[field];
        for (int faction = 0; faction < kFactionCount; ++faction)
            fieldMetrics.capitalPathLengths[faction] = AIPathLength(field, metrics.capitalTiles[faction]);

        for (int candidate = 0; candidate < kTileCount; ++candidate) {
            if (HexDistance(field, candidate) > 2) continue;
            if (candidate != field &&
                fieldMetrics.nearbyTileCount < static_cast<int>(fieldMetrics.nearbyTiles.size()))
                fieldMetrics.nearbyTiles[fieldMetrics.nearbyTileCount++] = candidate;
            const Tile& nearby = game.tiles[candidate];
            if (nearby.capitalFor >= 0 && nearby.capitalFor < kFactionCount)
                fieldMetrics.nearCapital[nearby.capitalFor] = true;
            if (nearby.estate == Estate::Town) fieldMetrics.nearTown = true;
        }
    }
    return metrics;
}

AIFactionTotals CalculateAIFactionTotals() {
    AIFactionTotals totals;
    for (const Tile& tile : game.tiles) {
        if (!tile.army.present || tile.army.owner < 0 || tile.army.owner >= kFactionCount) continue;
        totals.power[tile.army.owner] += tile.army.manpower + tile.army.morale;
    }
    return totals;
}

float FinalMoveProfitability(int from, int target, int faction, bool duel,
    const AIMapMetrics& metrics, const AIFactionTotals& totals, bool& waitForSupport) {
    const Tile& destination = game.tiles[target];
    const Tile& origin = game.tiles[from];
    const int attackPower = origin.army.manpower + origin.army.morale;
    float score = -10000000.0f;
    bool favorableCapitalCapture = false;

    for (int enemy = 0; enemy < kFactionCount; ++enemy) {
        if (enemy == faction) continue;
        float candidateScore = 0.0f;
        const int capital = metrics.capitalTiles[enemy];
        if (capital >= 0 && game.tiles[capital].owner == enemy) {
            candidateScore = -static_cast<float>(metrics.tiles[target].capitalPathLengths[enemy]);
            if (enemy == game.human) candidateScore += static_cast<float>(game.difficulty * 2);
        }
        if (game.pactPartner == faction && enemy == game.human && !duel)
            candidateScore -= 500.0f;
        score = std::max(score, candidateScore);
    }
    if (game.pactPartner == faction && destination.owner == game.human && !duel)
        score -= 500.0f;

    if (destination.land && destination.owner != faction) {
        if (destination.capitalFor >= 0 && destination.capitalFor == destination.owner &&
            destination.army.present && attackPower > destination.army.manpower + destination.army.morale) {
            score += 1000000.0f;
            favorableCapitalCapture = true;
        } else if (destination.capitalFor >= 0) {
            score += 20.0f;
        } else if (destination.estate == Estate::Town) {
            score += 5.0f;
        } else if (destination.estate == Estate::Port) {
            score += 3.0f;
        } else if (metrics.tiles[target].nearTown) {
            score += 3.0f;
        }
    }

    int friendlyNeighbourPower = 0;
    int enemyNeighbourPower = 0;
    const AITileMetrics& targetMetrics = metrics.tiles[target];
    for (int i = 0; i < targetMetrics.nearbyTileCount; ++i) {
        const Army& nearbyArmy = game.tiles[targetMetrics.nearbyTiles[i]].army;
        if (!nearbyArmy.present) continue;
        const int power = nearbyArmy.manpower + nearbyArmy.morale;
        if (nearbyArmy.owner == faction) friendlyNeighbourPower += power;
        else enemyNeighbourPower += power;
    }

    if (destination.army.present && destination.army.owner != faction) {
        if (targetMetrics.nearCapital[faction]) score += 1000.0f;
        if (destination.army.owner != game.human &&
            totals.power[destination.army.owner] > 1.5f * totals.power[faction] &&
            destination.army.manpower + destination.army.morale > attackPower &&
            ((destination.army.owner < 2 && faction < 2) ||
                (destination.army.owner > 1 && faction > 1))) {
            score += 200.0f;
        }
        if (game.difficulty > 5 && destination.army.owner != game.human)
            score -= 250.0f;
    }
    if (destination.army.present && destination.army.owner == faction &&
        destination.army.manpower > origin.army.manpower && destination.army.manpower < 70) {
        score += 2.0f;
    }
    if (origin.capitalFor == faction && !destination.army.present && game.turns < 5)
        score += 50.0f;

    const bool attackingStrongerArmy = destination.army.present &&
        attackPower < destination.army.manpower + destination.army.morale && origin.army.manpower < 90;
    if (((friendlyNeighbourPower < enemyNeighbourPower && friendlyNeighbourPower < 300) ||
            attackingStrongerArmy) && !targetMetrics.nearCapital[faction] && !favorableCapitalCapture) {
        if (game.aiSupportTargets[faction] == target) {
            if (game.aiSupportWaitCounts[faction] < 5) waitForSupport = true;
            else score -= 5.0f;
        } else {
            waitForSupport = true;
        }
    }
    return score;
}

std::vector<AIMoveChoice> CalculateArmyProfitabilities(int faction, bool duel,
    const AIMapMetrics& metrics, const AIFactionTotals& totals) {
    std::vector<AIMoveChoice> choices;
    for (int x = 0; x < kMapWidth; ++x) {
        for (int y = 0; y < kMapHeight; ++y) {
            const int from = Index(x, y);
            const Army& army = game.tiles[from].army;
            if (!army.present || army.owner != faction || army.moved) continue;
            const std::vector<int> candidates = PossibleMoves(from, true);
            std::vector<AIMoveChoice> rankedMoves;
            rankedMoves.reserve(candidates.size());
            for (int target : candidates) {
                bool waitForSupport = false;
                const float score = FinalMoveProfitability(from, target, faction, duel,
                    metrics, totals, waitForSupport);
                rankedMoves.push_back({ from, target, army.manpower + army.morale,
                    score, waitForSupport });
            }
            if (rankedMoves.empty()) continue;
            FlashArraySort(rankedMoves, CompareAIMoves);
            AIMoveChoice bestMove = rankedMoves.front();
            if (game.tiles[from].capitalFor == faction && game.turns > 5)
                bestMove.profitability -= 1000.0f;
            choices.push_back(bestMove);
        }
    }
    FlashArraySort(choices, CompareAIArmies);
    return choices;
}

float AIDistance(int first, int second) {
    const int ax = TileX(first);
    const int bx = TileX(second);
    const float px = static_cast<float>((ax - bx) * 5);
    const float ay = static_cast<float>(TileY(first) * 10 + (ax % 2 ? 5 : 0));
    const float by = static_cast<float>(TileY(second) * 10 + (bx % 2 ? 5 : 0));
    const float py = ay - by;
    return std::sqrt(px * px + py * py);
}

std::vector<AIMoveChoice> FindSupportArmies(int faction, const AIMoveChoice& primary) {
    std::vector<AIMoveChoice> supportChoices;
    for (int x = 0; x < kMapWidth; ++x) {
        for (int y = 0; y < kMapHeight; ++y) {
            const int from = Index(x, y);
            const Army& army = game.tiles[from].army;
            if (!army.present || army.owner != faction || army.moved || from == primary.from ||
                game.tiles[from].capitalFor == faction) continue;

            std::vector<AIMoveChoice> rankedMoves;
            for (int target : PossibleMoves(from, true)) {
                if (target == primary.target) continue;
                const Tile& destination = game.tiles[target];
                if (destination.army.present && destination.army.owner != faction) continue;
                rankedMoves.push_back({ from, target, army.manpower + army.morale,
                    -AIDistance(target, primary.target), false });
            }
            if (!rankedMoves.empty()) {
                FlashArraySort(rankedMoves, CompareAIMoves);
                supportChoices.push_back(rankedMoves.front());
            }
        }
    }
    FlashArraySort(supportChoices, CompareAIArmies);
    return supportChoices;
}

void BreakPactIfNeeded(int faction, int targetOwner) {
    if (game.pactPartner < 0) return;
    const bool humanBroke = faction == game.human && targetOwner == game.pactPartner;
    const bool partnerBroke = faction == game.pactPartner && targetOwner == game.human;
    if (!humanBroke && !partnerBroke) return;
    const int betrayed = humanBroke ? game.pactPartner : game.human;
    AddMoraleToFaction(betrayed, 30);
    AddNews(kFactions[game.pactPartner] + " broken!", NewspaperArticle::PactBroken, 4);
    game.pactPartner = -1;
    game.choosingPact = false;
}

// attack()'s losing branch: the defender loses manpower in proportion to the
// attacker's power, keeping at least one man.
Army RepelledDefender(const Army& attacker, const Army& defender) {
    const int attackPower = attacker.manpower + attacker.morale;
    const int defendPower = defender.manpower + defender.morale;
    const int casualties = static_cast<int>(std::floor(
        (static_cast<float>(attackPower) / std::max(1, defendPower)) * attacker.manpower));
    Army result = defender;
    result.manpower = std::max(1, defender.manpower - casualties);
    result.morale = std::min(defender.morale, result.manpower);
    return result;
}

bool AttackIsRepelled(const Army& attacker, const Army& defender) {
    return attacker.manpower + attacker.morale <= defender.manpower + defender.morale;
}

void MoveArmy(int from, int target) {
    Tile& source = game.tiles[from];
    Tile& destination = game.tiles[target];
    Army attacker = source.army;
    const int faction = attacker.owner;
    const int oldOwner = destination.owner;
    BreakPactIfNeeded(faction, oldOwner);
    source.army = Army{};
    attacker.moved = true;

    game.repelledDefenderTile = -1;

    if (destination.army.present && destination.army.owner != faction) {
        Army defender = destination.army;
        const int attackPower = attacker.manpower + attacker.morale;
        const int defendPower = defender.manpower + defender.morale;
        if (!AttackIsRepelled(attacker, defender)) {
            // setExplosion(army1, army2): the defender's clip explodes above
            // the arriving attacker.
            StartExplosion(target, defender, true);
            AddMoraleToFaction(defender.owner, -(defender.manpower / 10));
            const int casualties = static_cast<int>(std::floor((static_cast<float>(defendPower) / attackPower) * attacker.manpower));
            attacker.manpower = std::max(1, attacker.manpower - casualties);
            attacker.morale = std::min(attacker.morale, attacker.manpower);
            destination.army = attacker;
        } else {
            // setExplosion(army1, army1): the attacker explodes below the defender.
            StartExplosion(target, attacker, false);
            AddMoraleToFaction(faction, -(attacker.manpower / 10));
            const Army survivor = RepelledDefender(attacker, defender);
            destination.army.manpower = survivor.manpower;
            destination.army.morale = survivor.morale;
            // A defended battle alone does not replace the newspaper in the SWF.
            CheckOutcome();
            return;
        }
    } else if (destination.army.present && destination.army.owner == faction) {
        Army defender = destination.army;
        const int total = defender.manpower + attacker.manpower;
        const int merged = std::min(99, total);
        const int mergedFromAttacker = std::min(attacker.manpower, merged - defender.manpower);
        const int moralePoints = defender.manpower * defender.morale + mergedFromAttacker * attacker.morale;
        destination.army.manpower = merged;
        destination.army.morale = merged > 0 ? std::clamp(moralePoints / merged, 0, merged) : 0;
        destination.army.moved = true;
        if (total > merged) {
            attacker.manpower = total - merged;
            attacker.morale = std::min(attacker.morale, attacker.manpower);
            // joinUnits re-creates the overflow as a new army on the origin.
            // updateArmy never sets that clip's moved flag, so it may move again.
            attacker.blinkElapsed = 0.0f;
            attacker.moved = false;
            source.army = attacker;
        }
        AnnexLand(target, faction, &destination.army, false);
        CheckOutcome();
        return;
    } else {
        destination.army = attacker;
    }

    // annexLand on a capital, in the SWF's order: moraleEarned ends the game
    // when the human takes any capital while already holding two provinces;
    // a kingdom losing its own capital first frees its provinces, and the next
    // updateBoard then eliminates it.
    const bool capitalTaken = destination.capitalFor >= 0 && oldOwner != faction;
    if (capitalTaken && faction == game.human && ProvinceCount(game.human) >= 2)
        game.humanConquered = true;
    const bool sovereignCapitalLost = capitalTaken && oldOwner >= 0 && oldOwner == destination.capitalFor;
    if (sovereignCapitalLost) LiberateProvinces(oldOwner);
    AnnexLand(target, faction, &destination.army, false);
    if (sovereignCapitalLost)
        EliminateFaction(oldOwner, faction);
    CheckOutcome();
}

int HumanTerritoryArea() {
    int area = 0;
    for (const Tile& tile : game.tiles)
        if (tile.land && tile.owner == game.human) ++area;
    return area;
}

// updateBoard's human_condition: 0 victory close, 1 even, 2 weaker than a
// rival, 3 far weaker. Power is average morale plus total manpower.
int HumanCondition() {
    const int power = ManpowerTotal(game.human) + FactionRecruitMorale(game.human);
    const int provinces = ProvinceCount(game.human);
    int condition = 1;
    for (int faction = 0; faction < kFactionCount; ++faction) {
        if (faction == game.human || !IsAlive(faction)) continue;
        const int enemyPower = ManpowerTotal(faction) + FactionRecruitMorale(faction);
        if (power < 0.3f * enemyPower) condition = 3;
        else if (condition < 3 && power < 0.6f * enemyPower) condition = 2;
        else if (provinces >= 2 && power > 2 * enemyPower) condition = 0;
    }
    return condition;
}

void PublishDefaultNews() {
    game.newsPriority = 0;
    const int area = HumanTerritoryArea();
    if (area < game.humanTurnStartArea)
        AddNews("Strategic retreat.", NewspaperArticle::StrategicRetreat);
    else if (area > game.humanTurnStartArea)
        AddNews(kFactions[game.human] + " forces advance!", NewspaperArticle::Advance);
    else {
        const int condition = HumanCondition();
        if (condition == 0) AddNews("Victory is close!", NewspaperArticle::VictoryClose);
        else if (condition == 3) AddNews("The war is not lost yet!", NewspaperArticle::WarNotLost);
        else AddNews("Marika Rock sings for our troops!", NewspaperArticle::Marika);
    }
}

void FinishTurn(int faction) {
    for (Tile& tile : game.tiles) {
        if (!tile.army.present || tile.army.owner != faction) continue;
        if (!tile.army.moved) AddMorale(tile.army, -1);
        tile.army.moved = false;
    }
    SpawnRecruits(faction);
    CheckOutcome();
}

void StartArmyTravel(int from, int target);

bool BeginAIMove(int faction) {
    const AIMapMetrics metrics = BuildAIMapMetrics();
    int activeCapitals = 0;
    for (int capital = 0; capital < kFactionCount; ++capital)
        if (metrics.capitalTiles[capital] >= 0 && game.tiles[metrics.capitalTiles[capital]].owner == capital)
            ++activeCapitals;
    const bool duel = activeCapitals < 3;

    {
        const AIFactionTotals totals = CalculateAIFactionTotals();
        std::vector<AIMoveChoice> choices = CalculateArmyProfitabilities(faction, duel, metrics, totals);
        if (choices.empty()) return false;

        const AIMoveChoice& primary = choices.front();
        if (!primary.waitForSupport) {
            game.aiSupportTargets[faction] = -1;
            game.aiSupportWaitCounts[faction] = 0;
            StartArmyTravel(primary.from, primary.target);
            return true;
        }

        if (game.aiSupportTargets[faction] == primary.target) {
            ++game.aiSupportWaitCounts[faction];
        } else {
            game.aiSupportTargets[faction] = primary.target;
            game.aiSupportWaitCounts[faction] = 0;
        }

        const std::vector<AIMoveChoice> supportChoices = FindSupportArmies(faction, primary);
        const AIMoveChoice& choice = supportChoices.empty() ? primary : supportChoices.front();
        StartArmyTravel(choice.from, choice.target);
        return true;
    }
}

// Sprite 81 frame 4 runs sign_pact.inactive(hw_pact_signed && !duel) as the
// human's turn starts; a duel is fewer than three kingdoms holding their capital.
void RefreshPactButton() {
    int sovereignCapitals = 0;
    for (int faction = 0; faction < kFactionCount; ++faction)
        if (IsAlive(faction)) ++sovereignCapitals;
    game.pactButtonInactive = game.pactSigned && sovereignCapitals >= 3;
}

// Sprite 81's loop runs move_loop and game_dispatch every 12 fps frame. A
// computer party waits `wait` frames before each makeMove: 6 after a move
// (7 frames from one order to the next) and 12 at a turn change (14 frames
// from the previous party's last order). The clone resolves a move when its
// army lands, 7 frames after the order, so these delays count from there.
constexpr float kAIMoveFrame = 1.0f / 12.0f;
constexpr float kAITurnChangeAfterMove = 7.0f / 12.0f;
constexpr float kAITurnChangeAfterEndTurn = 14.0f / 12.0f;

// Sprite 81 frame 4 for `party`: move points capped at its army count; the
// human also gets a turn counter step, the pact button state and the army
// hint, which sprite 81 frame 5 only shows on the human's turn.
void BeginPartyTurn(int party, float aiWait) {
    game.currentPlayer = party;
    game.selectedTile = -1;
    game.moveTargets.clear();
    game.movesLeft = std::min(kMoveLimit, ArmyCount(party));
    if (party != game.human) {
        game.aiMovePause = aiWait;
        return;
    }
    ++game.turns;
    game.humanTurnStartArea = HumanTerritoryArea();
    RefreshPactButton();
    if (game.armyHintPending) {
        game.armyHintPending = false;
        game.armySelectionHintVisible = true;
    }
}

void StartNextTurn(float aiWait = kAITurnChangeAfterMove) {
    if (CheckOutcome()) return;
    const int startingPlayer = game.currentPlayer;
    for (int step = 1; step <= kFactionCount; ++step) {
        const int next = (startingPlayer + step) % kFactionCount;
        if (!IsAlive(next)) continue;
        BeginPartyTurn(next, aiWait);
        return;
    }
    if (game.screen == Screen::Playing && IsAlive(game.human))
        BeginPartyTurn(game.human, aiWait);
}

int EffectsRandom(int count);

void StartArmyTravel(int from, int target) {
    game.movingArmyFrom = from;
    game.movingArmyTarget = target;
    game.armyTravelElapsed = 0.0f;
    // The SWF's moveArmy plays s_war (rand(3) == 0) or s_gun as soon as an
    // army is sent into an enemy army, and the army clip plays s_szur once
    // when it starts sliding toward its new field.
    const Army& mover = game.tiles[from].army;
    const Army& defender = game.tiles[target].army;
    if (defender.present && defender.owner != mover.owner) {
        SoundEffect* battle = game.blankMapNumber || EffectsRandom(3) == 0
            ? gameAudio.battleWar.get() : gameAudio.battleGun.get();
        if (battle) battle->play();
        if (AttackIsRepelled(mover, defender)) {
            game.repelledDefenderTile = target;
            game.repelledDefender = RepelledDefender(mover, defender);
        }
    }
    if (gameAudio.armyMove) gameAudio.armyMove->play();
}

bool OrderPlayerMove(int from, int target) {
    if (!CanSelectPlayerArmy(from) || game.queuedArmyFrom >= 0) return false;
    const auto targets = PossibleMoves(from);
    if (std::find(targets.begin(), targets.end(), target) == targets.end()) return false;
    if (game.movingArmyFrom >= 0) {
        game.queuedArmyFrom = from;
        game.queuedArmyTarget = target;
    } else {
        StartArmyTravel(from, target);
    }
    return true;
}

void ClearBattleScars() {
    game.battleScars.clear();
    ++game.battleScarGeneration;
}

int EffectsRandom(int count) {
    return std::uniform_int_distribution<int>(0, count - 1)(game.effectsRandom);
}

void AddBattleScar(int tile) {
    // Sprite 167 frame 1 draws f1, f2 and r; the erosion clip flips then
    // rotates e_6.png with them before using it as the copyPixels mask.
    BattleScar scar;
    scar.tile = tile;
    scar.flipHorizontal = EffectsRandom(2) != 0;
    scar.flipVertical = EffectsRandom(2) != 0;
    scar.rotation = EffectsRandom(360);
    const int x = TileX(tile);
    const int y = TileY(tile);
    const glm::vec2 center(25.0f + x * 37.5f, 20.0f + y * 40.0f + (x % 2 ? 20.0f : 0.0f));
    for (BattleScarGrave& grave : scar.graves) {
        grave.variant = EffectsRandom(6);
        grave.position = glm::vec2(center.x + 25.0f - EffectsRandom(50) - 7.0f,
            center.y + 20.0f - EffectsRandom(40) - 15.0f);
    }
    game.battleScars.push_back(scar);
}

void UpdateBattleExplosions(float deltaTime) {
    if (game.showHelp) return;
    for (auto& explosion : game.explosions) {
        explosion.elapsed += std::max(0.0f, deltaTime);
        if (!explosion.scarred && explosion.elapsed >= kBattleScarDelaySeconds) {
            explosion.scarred = true;
            AddBattleScar(explosion.tile);
        }
    }
    game.explosions.erase(std::remove_if(game.explosions.begin(), game.explosions.end(),
        [](const BattleExplosion& explosion) { return explosion.elapsed >= kExplosionFrames / 12.0f; }),
        game.explosions.end());
}

// Resolve player and AI moves through the same animation and arrival path.
void EndHumanTurn(bool publishNews = true);

void UpdateTurnPlayback(float deltaTime) {
    if (game.screen != Screen::Playing || game.showHelp) return;
    const float elapsed = std::max(0.0f, deltaTime);
    if (game.movingArmyFrom >= 0) {
        game.armyTravelElapsed += elapsed;
        if (game.armyTravelElapsed < 7.0f / 12.0f) return;
        const int from = game.movingArmyFrom;
        const int target = game.movingArmyTarget;
        game.movingArmyFrom = game.movingArmyTarget = -1;
        game.newsPriority = 0;
        const unsigned int newsBefore = game.newsSerial;
        game.collectingMoveNews = true;
        MoveArmy(from, target);
        game.collectingMoveNews = false;
        if (game.currentPlayer == game.human && game.newsSerial == newsBefore) PublishDefaultNews();
        game.newsPriority = 0;
        --game.movesLeft;
        // Sprite 81 frame 6 lowers move points to the armies still able to
        // move, so lost or merged armies cannot leave unusable moves behind.
        game.movesLeft = std::min(game.movesLeft, MovableArmyCount(game.currentPlayer));
        // The army's 7-frame travel already covers sprite 81's 6-frame wait.
        game.aiMovePause = 0.0f;
        // A second order may have been clicked during travel. Validate it on
        // the resolved board, since combat/merging can change its legal moves.
        const int queuedFrom = game.queuedArmyFrom;
        const int queuedTarget = game.queuedArmyTarget;
        game.queuedArmyFrom = game.queuedArmyTarget = -1;
        if (queuedFrom >= 0) {
            // The SWF resolves every order as it is clicked, so a queued
            // order still runs when END TURN was pressed after it.
            const Army& queued = game.tiles[queuedFrom].army;
            const auto targets = PossibleMoves(queuedFrom);
            if (game.movesLeft > 0 && queued.present && queued.owner == game.human && !queued.moved &&
                std::find(targets.begin(), targets.end(), queuedTarget) != targets.end())
                StartArmyTravel(queuedFrom, queuedTarget);
        }
        if (game.selectedTile >= 0) {
            if (CanSelectPlayerArmy(game.selectedTile))
                game.moveTargets = PossibleMoves(game.selectedTile);
            else {
                game.selectedTile = -1;
                game.moveTargets.clear();
            }
        }
        if (game.currentPlayer != game.human) return;
        if (game.movesLeft <= 0) game.endTurnRequested = true;
    }
    if (game.currentPlayer == game.human) {
        // END TURN sets move_points to 0 and sprite 81's loop ends the turn on
        // its next tick. Its dialogs stop() that loop, so wait for them too.
        if (game.endTurnRequested && game.movingArmyFrom < 0 &&
            !game.showSpeechDialog && !game.choosingPact && game.modalDialog == ModalDialogKind::None)
            EndHumanTurn(false);
        return;
    }
    game.aiMovePause = std::max(0.0f, game.aiMovePause - elapsed);
    if (game.aiMovePause > 0.0f) return;
    if (game.movesLeft > 0 && BeginAIMove(game.currentPlayer)) return;
    FinishTurn(game.currentPlayer);
    StartNextTurn();
}

void EndHumanTurn(bool publishNews) {
    if (game.screen != Screen::Playing || game.currentPlayer != game.human) return;
    if (game.movingArmyFrom >= 0) {
        // Let the travelling army land; its move publishes the news.
        game.endTurnRequested = true;
        game.selectedTile = -1;
        game.moveTargets.clear();
        return;
    }
    game.endTurnRequested = false;
    game.selectedTile = -1;
    game.moveTargets.clear();
    FinishTurn(game.human);
    if (publishNews && game.screen == Screen::Playing) PublishDefaultNews();
    // Without a landing army this is END TURN itself, so the next party gets
    // the full 14 frames; otherwise the last move's travel already took 7.
    StartNextTurn(publishNews ? kAITurnChangeAfterEndTurn : kAITurnChangeAfterMove);
}

// generateMap with rnd_seed NaN. rand(10) <= 1 is !(1 < NaN), true, so every
// field is land. Each town draw reads hw_lands[i][NaN], which is undefined:
// it pushes undefined into hw_towns and marks no field, so only the four
// capitals exist and no path finds a port. The ld_/l_/c_/cd_NaN.png bitmaps
// fail to load, leaving the board transparent over shape 542.
void GenerateBlankMap() {
    for (Tile& tile : game.tiles) tile.land = true;
    for (int faction = 0; faction < kFactionCount; ++faction) {
        const int capitalIndex = Index(kCapitals[faction][0], kCapitals[faction][1]);
        Tile& capital = game.tiles[capitalIndex];
        capital.estate = Estate::Town;
        capital.capitalFor = faction;
        capital.owner = faction;
        for (int neighbour : Neighbours(capitalIndex)) {
            Tile& tile = game.tiles[neighbour];
            if (tile.estate == Estate::None) tile.owner = faction;
        }
    }
    // randTown(): towns[NaN] starts undefined and then trails one name
    // behind, so in x-major order the capitals get undefined, "Abu Dhabi",
    // "Abuja" and "Accra".
    int nameIndex = -1;
    for (int x = 0; x < kMapWidth; ++x) {
        for (int y = 0; y < kMapHeight; ++y) {
            Tile& tile = At(x, y);
            if (tile.estate != Estate::Town) continue;
            if (nameIndex >= 0) tile.townName = kCityNames[static_cast<size_t>(nameIndex)];
            ++nameIndex;
        }
    }
}

void GenerateMap(int mapNumber) {
    game.explosions.clear();
    ClearBattleScars();
    game.movingArmyFrom = game.movingArmyTarget = -1;
    game.queuedArmyFrom = game.queuedArmyTarget = -1;
    game.repelledDefenderTile = -1;
    game.endTurnRequested = false;
    game.humanConquered = false;
    game.armyTravelElapsed = game.aiMovePause = 0.0f;
    game.random.seed(static_cast<std::mt19937::result_type>(mapNumber));
    game.aiSupportTargets.fill(-1);
    game.aiSupportWaitCounts.fill(0);
    game.capitalTooltipSecondsRemaining = kCapitalTooltipDurationSeconds;
    game.capitalTooltipTimerStarted = false;
    // RANDOM MAP, CHANGE MAP and a finished game all replay from "generate".
    game.menuElapsed = 0.0f;
    game.tiles.fill(Tile{});
    game.cityNameAlpha.fill(0.0f);
    game.newsPriority = 0;
    game.collectingMoveNews = false;
    game.news.clear();
    game.newspaperArticle = NewspaperArticle::Peace;
    game.pactPartner = -1;
    game.choosingPact = false;
    game.pactDialogPage = PactDialogPage::Choose;
    game.pactSigned = false;
    game.pactButtonInactive = false;
    game.speechUsed = false;
    game.showSpeechDialog = false;
    game.selectedTile = -1;
    game.moveTargets.clear();

    if (game.blankMapNumber) {
        GenerateBlankMap();
        return;
    }

    std::int64_t legacySeed = mapNumber;
    for (int x = 0; x < 6; ++x) {
        for (int y = 0; y < 4; ++y) {
            TerrainPatch& patch = game.terrainPatches[static_cast<size_t>(y * 6 + x)];
            patch.dryLandVariant = LegacyMapRandom(legacySeed, 6);
            patch.landVariant = LegacyMapRandom(legacySeed, 6);
            patch.flipHorizontal = LegacyMapRandom(legacySeed, 2) != 0;
            patch.flipVertical = LegacyMapRandom(legacySeed, 2) != 0;
            patch.rotation = LegacyMapRandom(legacySeed, 4) * 90;
        }
    }

    for (int x = 0; x < kMapWidth; ++x) {
        for (int y = 0; y < kMapHeight; ++y) {
            Tile& tile = At(x, y);
            const bool capital = (x == 1 || x == kMapWidth - 2) && (y == 1 || y == kMapHeight - 2);
            tile.land = capital || LegacyMapRandom(legacySeed, 10) <= 1;
            tile.dry = false;
        }
    }

    std::array<bool, kTileCount> initialLand{};
    for (int i = 0; i < kTileCount; ++i) initialLand[i] = game.tiles[i].land;
    for (int i = 0; i < kTileCount; ++i) {
        if (initialLand[i]) continue;
        for (int neighbour : Neighbours(i)) {
            if (initialLand[neighbour]) {
                game.tiles[i].land = true;
                break;
            }
        }
    }
    // The SWF removes isolated water cells in-place in x-major order. The
    // order matters: once a cell is changed to land, later cells see that
    // update, which can change the coastline and every subsequent sea-art
    // random draw. Keep this scan in the same order as its nested AS loops.
    for (int x = 0; x < kMapWidth; ++x) {
        for (int y = 0; y < kMapHeight; ++y) {
            const int index = Index(x, y);
            if (game.tiles[index].land) continue;
            bool hasWaterNeighbour = false;
            for (int neighbour : Neighbours(index))
                if (!game.tiles[neighbour].land) hasWaterNeighbour = true;
            if (!hasWaterNeighbour) game.tiles[index].land = true;
        }
    }
    std::vector<int> towns;
    towns.reserve(40);
    for (int faction = 0; faction < kFactionCount; ++faction) {
        const int capitalIndex = Index(kCapitals[faction][0], kCapitals[faction][1]);
        Tile& capital = game.tiles[capitalIndex];
        capital.estate = Estate::Town;
        capital.capitalFor = faction;
        capital.owner = faction;
        towns.push_back(Index(kCapitals[faction][0], kCapitals[faction][1]));
        for (int neighbour : Neighbours(capitalIndex)) {
            Tile& tile = game.tiles[neighbour];
            if (tile.land && tile.estate == Estate::None) tile.owner = faction;
        }
    }

    std::vector<std::vector<int>> landGroups;
    std::array<bool, kTileCount> visited{};
    for (int x = 0; x < kMapWidth; ++x) {
        for (int y = 0; y < kMapHeight; ++y) {
            const int start = Index(x, y);
            if (!game.tiles[start].land || visited[start]) continue;
            std::vector<int> group;
            std::vector<int> queue{ start };
            visited[start] = true;
            for (size_t cursor = 0; cursor < queue.size(); ++cursor) {
                const int current = queue[cursor];
                group.push_back(current);
                for (int neighbour : Neighbours(current)) {
                    if (game.tiles[neighbour].land && !visited[neighbour]) {
                        visited[neighbour] = true;
                        queue.push_back(neighbour);
                    }
                }
            }
            landGroups.push_back(std::move(group));
        }
    }

    for (const std::vector<int>& group : landGroups) {
        const int townGoal = static_cast<int>(group.size()) / 10 + 1;
        for (int town = 0; town < townGoal; ++town) {
            bool created = false;
            int attempts = 0;
            while (!created) {
                ++attempts;
                if (attempts > 10) created = true;
                const int candidate = group[static_cast<size_t>(LegacyMapRandom(legacySeed, static_cast<int>(group.size())))];
                Tile& tile = game.tiles[candidate];
                if (tile.estate != Estate::None) continue;
                bool inland = true;
                for (int neighbour : Neighbours(candidate)) {
                    if (!game.tiles[neighbour].land || game.tiles[neighbour].estate != Estate::None) {
                        inland = false;
                        break;
                    }
                }
                if (inland) {
                    tile.estate = Estate::Town;
                    towns.push_back(candidate);
                    created = true;
                }
            }
        }
    }

    for (size_t i = 0; i < towns.size(); ++i) {
        const size_t swapIndex = static_cast<size_t>(LegacyMapRandom(legacySeed, static_cast<int>(towns.size())));
        std::swap(towns[i], towns[swapIndex]);
    }

    int portCount = 0;
    auto findPath = [&](int start, int end, bool avoidWater) {
        struct PathNode {
            int field = -1;
            int parent = -1;
            int travelCost = 0;
            float distanceCost = 0.0f;
        };

        if (!game.tiles[start].land) avoidWater = false;
        std::array<int, kTileCount> openIndex;
        std::array<int, kTileCount> closedIndex;
        openIndex.fill(-1);
        closedIndex.fill(-1);
        std::vector<PathNode> open;
        std::vector<PathNode> closed;
        open.push_back({ start, -1, 0, 0.0f });
        openIndex[start] = 0;

        const auto distanceToGoal = [&](int field) {
            const int x = TileX(field);
            const int y = TileY(field);
            const int goalX = TileX(end);
            const int goalY = TileY(end);
            const float px = static_cast<float>((x - goalX) * 5);
            const float py = (static_cast<float>(y) + (x % 2 ? 0.5f : 0.0f) -
                static_cast<float>(goalY) - (goalX % 2 ? 0.5f : 0.0f)) * 10.0f;
            return std::sqrt(px * px + py * py);
        };

        while ((closed.empty() || closed.back().field != end) && !open.empty()) {
            const PathNode current = open.front();
            open.erase(open.begin());
            for (int neighbour : Neighbours(current.field)) {
                const Tile& from = game.tiles[current.field];
                const Tile& to = game.tiles[neighbour];
                bool canWalk = to.estate != Estate::Town;
                if (canWalk && avoidWater) {
                    canWalk = (!from.land && !to.land) || (from.land && to.land) ||
                        (!from.land && to.land) || (!to.land && from.estate == Estate::Port);
                }
                if (!canWalk && neighbour != end) continue;

                PathNode next{ neighbour, current.field, current.travelCost + 5, 5.0f + distanceToGoal(neighbour) };
                if (closedIndex[neighbour] >= 0) {
                    PathNode& old = closed[static_cast<size_t>(closedIndex[neighbour])];
                    if (old.travelCost > next.travelCost) old = next;
                } else if (openIndex[neighbour] < 0) {
                    openIndex[neighbour] = static_cast<int>(open.size());
                    open.push_back(next);
                }
            }
            closedIndex[current.field] = static_cast<int>(closed.size());
            closed.push_back(current);

            if (!open.empty()) {
                size_t best = 0;
                for (size_t i = 1; i < open.size(); ++i)
                    if (open[i].distanceCost < open[best].distanceCost) best = i;
                std::swap(open.front(), open[best]);
            }
        }

        std::vector<int> path;
        if (closed.empty() || closed.back().field != end) return path;
        int field = end;
        while (field != start) {
            path.push_back(field);
            const int closedNode = closedIndex[field];
            if (closedNode < 0) return std::vector<int>{};
            field = closed[static_cast<size_t>(closedNode)].parent;
            if (field < 0) return std::vector<int>{};
        }
        path.push_back(start);
        std::reverse(path.begin(), path.end());
        return path;
    };

    for (size_t town = 0; town + 1 < towns.size(); ++town) {
        std::vector<int> path = findPath(towns[town], towns[town + 1], true);
        if (path.empty() || static_cast<int>(path.size()) > portCount)
            path = findPath(towns[town], towns[town + 1], false);
        for (size_t step = 1; step + 1 < path.size(); ++step) {
            const int current = path[step];
            if (game.tiles[current].land && !game.tiles[path[step + 1]].land) {
                game.tiles[current].estate = Estate::Port;
                ++portCount;
            }
            if (game.tiles[current].land && !game.tiles[path[step - 1]].land) {
                game.tiles[current].estate = Estate::Port;
                ++portCount;
            }
        }
    }

    for (int x = 0; x < kMapWidth; ++x) {
        for (int y = 0; y < kMapHeight; ++y) {
            Tile& tile = At(x, y);
            if (!tile.land) {
                const SeaArtSelection selection = SelectSeaArtLikeSource(legacySeed);
                tile.artVariant = selection.variant;
                tile.artFlipHorizontal = selection.flipHorizontal;
                tile.artFlipVertical = selection.flipVertical;
                tile.artRotation = selection.rotation;
            }
        }
    }

    std::vector<std::string> openCities = kCityNames;
    for (int x = 0; x < kMapWidth; ++x) {
        for (int y = 0; y < kMapHeight; ++y) {
            Tile& tile = At(x, y);
            if (tile.estate != Estate::Town && tile.estate != Estate::Port) continue;
            tile.dryArtVariant = LegacyMapRandom(legacySeed, 6);
            tile.artVariant = LegacyMapRandom(legacySeed, 6);
            tile.artFlipHorizontal = LegacyMapRandom(legacySeed, 2) != 0;
            tile.artFlipVertical = LegacyMapRandom(legacySeed, 2) != 0;
            tile.artRotation = LegacyMapRandom(legacySeed, 360);
            const int cityIndex = LegacyMapRandom(legacySeed, static_cast<int>(openCities.size()));
            std::swap(openCities[0], openCities[static_cast<size_t>(cityIndex)]);
            tile.townName = openCities.front();
            openCities.erase(openCities.begin());
        }
    }

}

void UpdateTerritoryFade(float deltaTime, bool snap = false) {
    // Sprite 367 fades between frames 5 and 8 (and 13 and 16) at 12 fps.
    // Movement already supplies the source timeline's initial delay.
    const float step = std::max(0.0f, deltaTime) * 4.0f;
    for (int index = 0; index < kTileCount; ++index) {
        for (int faction = 0; faction < kFactionCount; ++faction) {
            const float target = game.tiles[index].land && game.tiles[index].owner == faction ? 1.0f : 0.0f;
            float& alpha = game.territoryAlpha[index][faction];
            if (snap) alpha = target;
            else if (alpha < target) alpha = std::min(target, alpha + step);
            else alpha = std::max(target, alpha - step);
        }
    }
}

void StartCampaign(int faction) {
    game.explosions.clear();
    ClearBattleScars();
    game.endTurnRequested = false;
    game.humanConquered = false;
    game.selectedFaction = std::clamp(faction, 0, kFactionCount - 1);
    game.previewFaction = game.selectedFaction;
    game.human = game.selectedFaction;
    game.turns = 0;
    game.gameplayElapsed = 0.0f;
    game.fieldDoom.fill(0.0f);
    game.aiSupportTargets.fill(-1);
    game.aiSupportWaitCounts.fill(0);
    game.screen = Screen::Playing;
    StartGameplayMusic();
    game.armySelectionHintVisible = false;
    game.armyHintPending = true;
    game.armyMoveHintElapsed = -1.0f;
    game.armyHintTile = -1;
    game.movesLeftHintVisible = true;
    game.specialButtonsHintVisible = true;
    game.helpHintVisible = true;
    game.openingTooltipSecondsRemaining = kOpeningTooltipDurationSeconds;
    game.showHelp = false;
    game.helpPage = 0;
    if (game.mapNumberInput) game.mapNumberInput->hasFocus = false;
    game.newsPriority = 0;
    game.collectingMoveNews = false;
    game.news.clear();
    AddNews("Peace is not worth the effort!", NewspaperArticle::Peace);
    for (Tile& tile : game.tiles) {
        tile.owner = -1;
        tile.army = Army{};
    }
    for (int player = 0; player < kFactionCount; ++player) {
        const int capitalIndex = Index(kCapitals[player][0], kCapitals[player][1]);
        AnnexLand(capitalIndex, player, nullptr, true);
    }
    for (int player = 0; player < kFactionCount; ++player)
        SpawnRecruits(player);
    // Sprite 81 frame 3: turn_party = 0 and wait = 0, so Redosia always opens
    // and the chosen country decides when the human first moves.
    game.pactSigned = false;
    BeginPartyTurn(0, kAIMoveFrame);
    UpdateTerritoryFade(0.0f, true);
}

struct BoardLayout {
    float scale;
    float left;
    float top;
};

// Sprite 232's board clip y on each of its frames (1-based): it rises from 80
// to 45 for country selection, drops back through 80 to 117 for play, and
// returns to 80 under the game-over panel.
constexpr std::array<float, 26> kBoardStageY = {
    80.0f, 80.0f, 80.0f, 80.0f, 80.0f, 80.0f, 80.0f, 71.2f, 62.5f, 53.8f, 45.0f,
    53.8f, 62.5f, 71.2f, 80.0f, 80.0f, 87.4f, 94.8f, 102.2f, 109.6f, 117.0f,
    109.6f, 102.2f, 94.8f, 87.4f, 80.0f };
constexpr int kPleaseWaitHoldFrame = 13;
constexpr int kCampaignStartFrame = 20;

int MenuTimelineFrame() {
    return std::min(10, 1 + static_cast<int>(game.menuElapsed * 12.0f));
}

int PreparingTimelineFrame() {
    return std::min(kCampaignStartFrame, 11 + static_cast<int>(game.preparingElapsed * 12.0f + 1e-3f));
}

float BoardStageY() {
    switch (game.screen) {
    case Screen::Menu: return kBoardStageY[static_cast<size_t>(MenuTimelineFrame())];
    case Screen::Preparing: return kBoardStageY[static_cast<size_t>(PreparingTimelineFrame())];
    case Screen::Result:
        // Sprite 231 arrives on frame 21 ("game_over").
        return kBoardStageY[static_cast<size_t>(std::min(25, 21 + static_cast<int>(game.resultElapsed * 12.0f)))];
    case Screen::Playing: break;
    }
    return 117.0f;
}

BoardLayout GetBoardLayout(int width, int height) {
    const float scale = std::clamp(std::min(width / 800.0f, height / 600.0f), 0.55f, 1.28f);
    const float stageOffsetX = (width - 800.0f * scale) * 0.5f;
    // The SWF's gameplay frame places the board clip at (17, 117).
    return { scale, stageOffsetX + 17.0f * scale, BoardStageY() * scale };
}
glm::vec2 TileCenter(int index, const BoardLayout& layout) {
    const int x = TileX(index);
    const int y = TileY(index);
    return glm::vec2(
        layout.left + 25.0f * layout.scale + x * 37.5f * layout.scale,
        layout.top + 20.0f * layout.scale + y * 40.0f * layout.scale + (x % 2 ? 20.0f * layout.scale : 0.0f)
    );
}

void DrawHexOutline(glm::vec2 center, float scale, glm::vec4 outline, float thickness) {
    const float halfW = 25.0f * scale;
    const float halfH = 20.0f * scale;
    const float shoulder = halfW * 0.5f;
    std::vector<glm::vec2> border = {
        {center.x - halfW, center.y}, {center.x - shoulder, center.y - halfH},
        {center.x + shoulder, center.y - halfH}, {center.x + halfW, center.y},
        {center.x + shoulder, center.y + halfH}, {center.x - shoulder, center.y + halfH},
        {center.x - halfW, center.y}
    };
    BatchDrawLines(border, outline, thickness * scale);
}

void DrawHexGridCell(glm::vec2 center, float scale) {
    if (artwork.hexGridOutline) {
        // Draw the source's 51x41 SVG as a regular quad. Mapping it through the
        // clipped hex fan distorts the straight edges into a triangular mesh.
        const float width = 51.0f * scale;
        const float height = 41.0f * scale;
        DrawRectangle(Rect(center.x - width * 0.5f, center.y - height * 0.5f,
            width, height), *artwork.hexGridOutline);
        return;
    }

    DrawHexOutline(center, scale, Color(255, 255, 255, 26), 1.0f);
}

void AppendHexFillVertices(std::vector<float>& vertices, glm::vec2 center,
    float scale, float inset = 0.0f) {
    const float halfW = std::max(0.0f, 25.0f * scale - inset);
    const float halfH = std::max(0.0f, 20.0f * scale - inset);
    const float shoulder = halfW * 0.5f;
    const std::array<glm::vec2, 6> corners = {{
        {center.x - halfW, center.y},
        {center.x - shoulder, center.y - halfH},
        {center.x + shoulder, center.y - halfH},
        {center.x + halfW, center.y},
        {center.x + shoulder, center.y + halfH},
        {center.x - shoulder, center.y + halfH}
    }};
    const auto appendVertex = [&](glm::vec2 point) {
        vertices.push_back(point.x);
        vertices.push_back(point.y);
        vertices.push_back(0.0f);
        vertices.push_back(0.0f);
        vertices.push_back(0.0f);
    };
    for (size_t side = 0; side < corners.size(); ++side) {
        appendVertex(center);
        appendVertex(corners[side]);
        appendVertex(corners[(side + 1) % corners.size()]);
    }
}

void DrawSolidTriangles(const std::vector<float>& vertices, glm::vec4 fill) {
    if (vertices.empty()) return;
    BeginShaderMode(extras::Default2DShader);
    extras::ActiveShader->setUniform("model", glm::mat4(1.0f));
    extras::ActiveShader->setUniform("useTexture", 0);
    extras::ActiveShader->setUniform("mixColor", false);
    extras::ActiveShader->setUniform("color", fill);
    glBindVertexArray(global_rectangle_vao);
    glBindBuffer(GL_ARRAY_BUFFER, global_rectangle_vbo);
    glBufferData(GL_ARRAY_BUFFER, vertices.size() * sizeof(float),
        vertices.data(), GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(vertices.size() / 5));
    if (!extras::optimizeGLCalls) {
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glBindVertexArray(0);
    }
    EndShaderMode();
}

template <typename ShouldDraw>
void DrawHexFillBatch(const BoardLayout& layout, glm::vec4 fill,
    ShouldDraw shouldDraw) {
    static std::vector<float> vertices;
    vertices.clear();
    vertices.reserve(kTileCount * 18 * 5);
    for (int index = 0; index < kTileCount; ++index) {
        if (!shouldDraw(index)) continue;
        AppendHexFillVertices(vertices, TileCenter(index, layout), layout.scale);
    }
    DrawSolidTriangles(vertices, fill);
}

bool Contains(Rect rect, glm::vec2 point) {
    return point.x >= rect.position.x && point.x <= rect.position.x + rect.size.x &&
        point.y >= rect.position.y && point.y <= rect.position.y + rect.size.y;
}

void AppendTexturedHexVertices(std::vector<float>& vertices, glm::vec2 center,
    float scale) {
    const float halfW = 25.0f * scale;
    const float halfH = 20.0f * scale;
    const float topY = center.y + (extras::drawYDown ? -halfH : halfH);
    const float bottomY = center.y + (extras::drawYDown ? halfH : -halfH);

    const std::array<glm::vec2, 6> positions = {{
        {center.x - halfW, center.y},
        {center.x - halfW * 0.5f, topY},
        {center.x + halfW * 0.5f, topY},
        {center.x + halfW, center.y},
        {center.x + halfW * 0.5f, bottomY},
        {center.x - halfW * 0.5f, bottomY}
    }};
    const std::array<glm::vec2, 6> texCoords = {{
        {0.0f, 0.5f}, {0.25f, 1.0f}, {0.75f, 1.0f},
        {1.0f, 0.5f}, {0.75f, 0.0f}, {0.25f, 0.0f}
    }};

    const auto writeVertex = [&](glm::vec2 position, glm::vec2 uv) {
        vertices.push_back(position.x);
        vertices.push_back(position.y);
        vertices.push_back(0.0f);
        vertices.push_back(uv.x);
        vertices.push_back(uv.y);
    };
    for (size_t i = 0; i < positions.size(); ++i) {
        const size_t next = (i + 1) % positions.size();
        writeVertex(center, glm::vec2(0.5f, 0.5f));
        writeVertex(positions[i], texCoords[i]);
        writeVertex(positions[next], texCoords[next]);
    }
}

void AppendTexturedRectVertices(std::vector<float>& vertices, Rect rect) {
    const float left = rect.position.x;
    const float right = left + rect.size.x;
    const float top = rect.position.y + (extras::drawYDown ? 0.0f : rect.size.y);
    const float bottom = rect.position.y + (extras::drawYDown ? rect.size.y : 0.0f);
    const auto writeVertex = [&](float x, float y, float u, float v) {
        vertices.push_back(x);
        vertices.push_back(y);
        vertices.push_back(0.0f);
        vertices.push_back(u);
        vertices.push_back(v);
    };
    writeVertex(left, top, 0.0f, 1.0f);
    writeVertex(left, bottom, 0.0f, 0.0f);
    writeVertex(right, bottom, 1.0f, 0.0f);
    writeVertex(right, bottom, 1.0f, 0.0f);
    writeVertex(right, top, 1.0f, 1.0f);
    writeVertex(left, top, 0.0f, 1.0f);
}

void DrawTexturedTriangles(const std::vector<float>& vertices, Texture& texture,
    glm::vec4 tint) {
    if (vertices.empty()) return;
    BeginShaderMode(extras::Default2DShader);
    extras::ActiveShader->setUniform("model", glm::mat4(1.0f));
    extras::ActiveShader->setUniform("textureSampler", 0);
    extras::ActiveShader->setUniform("useTexture", 1);
    extras::ActiveShader->setUniform("mixColor", true);
    extras::ActiveShader->setUniform("color", tint);
    texture.bind(0);

    glBindVertexArray(global_rectangle_vao);
    glBindBuffer(GL_ARRAY_BUFFER, global_rectangle_vbo);
    glBufferData(GL_ARRAY_BUFFER, vertices.size() * sizeof(float),
        vertices.data(), GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(vertices.size() / 5));

    texture.unbind();
    if (!extras::optimizeGLCalls) {
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glBindVertexArray(0);
    }
    EndShaderMode();
}

void DrawTexturedHex(glm::vec2 center, float scale, Texture& texture, glm::vec4 tint) {
    static std::vector<float> vertices;
    vertices.clear();
    vertices.reserve(18 * 5);
    AppendTexturedHexVertices(vertices, center, scale);
    DrawTexturedTriangles(vertices, texture, tint);
}

void DrawYellowTileHighlights(const BoardLayout& layout, const std::vector<int>& indices) {
    if (indices.empty()) return;

    // The source yellow fill is flat #ffff66. Rendering adjacent cells as one
    // solid mesh avoids transparent SVG edge seams between highlighted tiles.
    static std::vector<float> vertices;
    vertices.clear();
    vertices.reserve(indices.size() * 18 * 5);
    for (int index : indices)
        AppendHexFillVertices(vertices, TileCenter(index, layout), layout.scale);
    DrawSolidTriangles(vertices, Color(255, 255, 102, 145));
}

void DrawHexGridBatch(const BoardLayout& layout) {
    if (!artwork.hexGridOutline) {
        for (int index = 0; index < kTileCount; ++index)
            DrawHexGridCell(TileCenter(index, layout), layout.scale);
        return;
    }

    static std::vector<float> vertices;
    vertices.clear();
    vertices.reserve(kTileCount * 6 * 5);
    const float width = 51.0f * layout.scale;
    const float height = 41.0f * layout.scale;
    for (int index = 0; index < kTileCount; ++index) {
        const glm::vec2 center = TileCenter(index, layout);
        AppendTexturedRectVertices(vertices, Rect(center.x - width * 0.5f,
            center.y - height * 0.5f, width, height));
    }
    DrawTexturedTriangles(vertices, *artwork.hexGridOutline, Color(255, 255, 255));
}

Rect GetHexBoardFrameRect(const BoardLayout& layout) {
    const float scale = layout.scale;
    return Rect(layout.left - 23.5f * scale, layout.top - 124.0f * scale,
        818.0f * scale, 701.9f * scale);
}

void DrawHexBoardFrame(const BoardLayout& layout) {
    if (artwork.gameplayMapFrame) {
        DrawRectangle(GetHexBoardFrameRect(layout), *artwork.gameplayMapFrame);
    }
}

void DrawHexBoardEdge(const BoardLayout& layout) {
    if (artwork.gameplayMapEdge)
        DrawRectangle(GetHexBoardFrameRect(layout), *artwork.gameplayMapEdge);
}

void DrawGameplaySkyline(int width, int height) {
    const float scale = std::min(static_cast<float>(width) / 800.0f,
        static_cast<float>(height) / 600.0f);
    const float offset = (width - 800.0f * scale) * 0.5f;
    if (artwork.gameplaySkyline)
        DrawRectangle(Rect(offset, -10.0f * scale, 800.0f * scale, 61.4f * scale),
            *artwork.gameplaySkyline);
}
float SelectionUiScale(int width, int height) {
    return std::clamp(std::min(width / 800.0f, height / 600.0f), 0.72f, 1.2f);
}

float SelectionUiOffset(int width, float scale) {
    return (width - 800.0f * scale) * 0.5f;
}

float GetAudioToggleStageScale(int width, int height) {
    // Gameplay uses the SWF stage scale used by the board. Selection screens
    // retain their own stage sizing rules.
    return game.screen == Screen::Playing
        ? GetBoardLayout(width, height).scale
        : SelectionUiScale(width, height);
}

Rect GetAudioToggleRect(int width, int height) {
    constexpr float iconScale = 1.531f;
    const float scale = GetAudioToggleStageScale(width, height);
    const float stageOffset = SelectionUiOffset(width, scale);
    // Calibrated to the exported sprite frame's visible bounds.
    // The speaker belongs to the board clip (sprite 81), 47.1 px below its
    // origin, so it follows the board: y=80 while sprite 192 holds sprite 232
    // on frame 6, y=45 on the selection screen and y=117 in gameplay.
    const float stageY = BoardStageY() + 47.1f;
    return Rect(stageOffset + 774.5f * scale, stageY * scale,
        22.0f * iconScale * scale, 15.0f * iconScale * scale);
}

struct QualityTogglePlacement {
    glm::vec2 baseline;
    float fontSize;
    Rect hitRect;
};

QualityTogglePlacement GetQualityTogglePlacement(int width, int height) {
    // Button 295 sits below the speaker in sprite 359: static text "Q" in
    // Arial Bold 16 px, scaled 1.22. Its pen and baseline are measured
    // against the speaker from the SWF render.
    const Rect speaker = GetAudioToggleRect(width, height);
    const float scale = GetAudioToggleStageScale(width, height);
    const glm::vec2 baseline = speaker.position + glm::vec2(3.76f, 60.73f) * scale;
    return { baseline, 16.0f * 1.2227f * scale,
        Rect(baseline.x, baseline.y - 16.0f * scale, 16.0f * scale, 18.0f * scale) };
}

// Sprite 81 sits under every dialog clip, and each dialog's `modal` shape
// takes the mouse, so the speaker and Q only respond on an uncovered board.
bool AudioToggleCovered() {
    return game.modalDialog != ModalDialogKind::None || game.showHelp || game.showSpeechDialog ||
        game.choosingPact || game.screen == Screen::Preparing || game.screen == Screen::Result;
}

void DrawQualityToggle(int width, int height) {
    Text* renderer = extras::defaultTextRenderer;
    if (!renderer || !artwork.armyCounterFont) return;
    const QualityTogglePlacement placement = GetQualityTogglePlacement(width, height);
    const glm::vec2 normalizedMouse = GetMousePosition();
    const glm::vec2 mouse(normalizedMouse.x * width, normalizedMouse.y * height);
    // Static text 293 (up) is 50% white; 294 (over/down) is opaque.
    const bool hovered = !AudioToggleCovered() && Contains(placement.hitRect, mouse);
    Font* previousFont = renderer->font;
    const int previousWeight = renderer->fontWeight;
    const float previousStroke = renderer->strokeWidth;
    renderer->setFont(*artwork.armyCounterFont);
    // Flash's 50% white Q carries less ink than the outline renders at 400.
    renderer->setFontWeight(370);
    renderer->setStrokeWidth(0.0f);
    renderer->setCharacterSize(placement.fontSize);
    renderer->setColor(Color(255, 255, 255, hovered ? 255 : 128));
    renderer->setString("Q");
    renderer->DrawPrecise(placement.baseline.x, placement.baseline.y,
        TextAlignmentX::LEFT, TextAlignmentY::BASELINE);
    renderer->setStrokeWidth(previousStroke);
    renderer->setFontWeight(previousWeight);
    if (previousFont)
        renderer->setFont(*previousFont);
}

void DrawWebdingsGlyph(Font& font, char character, const Rect& frame,
    float authoredFontSize, float sourceLeft, float sourceTop, float scale,
    float iconScale, glm::vec4 color) {
    const auto glyph = font.characters.find(static_cast<char32_t>(character));
    if (glyph == font.characters.end() || font.metrics.emSize <= 0.0) return;

    const float fontSize = authoredFontSize * iconScale * scale;
    const float glyphScale = fontSize / static_cast<float>(font.metrics.emSize);
    const float glyphPadding = static_cast<float>(std::max(
        std::abs(font.emRangeLow), std::abs(font.emRangeHigh)));
    const float inkLeft = frame.position.x + sourceLeft * iconScale * scale;
    const float inkTop = frame.position.y + sourceTop * iconScale * scale;
    const float baselineX = inkLeft -
        (glyph->second.Bearing.x + glyphPadding) * glyphScale;
    const float baselineY = inkTop +
        (glyph->second.Bearing.y - glyphPadding) * glyphScale;

    Text iconText(font, std::string(1, character), fontSize);
    iconText.setColor(color);
    iconText.DrawPrecise(baselineX, baselineY,
        TextAlignmentX::LEFT, TextAlignmentY::BASELINE);
}

void DrawAudioToggle(int width, int height) {
    DrawQualityToggle(width, height);
    const Rect rect = GetAudioToggleRect(width, height);
    const float scale = GetAudioToggleStageScale(width, height);
    const glm::vec2 normalizedMouse = GetMousePosition();
    const glm::vec2 mouse(normalizedMouse.x * width, normalizedMouse.y * height);
    const bool hovered = !AudioToggleCovered() && Contains(rect, mouse);
    if (artwork.audioToggleFont) {
        // The SWF uses Webdings W for the speaker and x for the mute slash.
        // Its over/down state raises the speaker glyph opacity from 50% to 100%.
        DrawWebdingsGlyph(*artwork.audioToggleFont, 'W', rect, 14.0f, 4.0f, 3.0f,
            scale, 1.531f, Color(255, 255, 255, hovered ? 255 : 128));
        if (gameAudio.muted)
            DrawWebdingsGlyph(*artwork.audioToggleFont, 'x', rect, 16.0f, 0.0f, 2.0f,
                scale, 1.531f, Color(255, 255, 255, 128));
        return;
    }

    Texture* icon = gameAudio.muted ? artwork.soundOffIcon.get() : artwork.soundOnIcon.get();
    if (icon) DrawRectangle(rect, Color(255, 255, 255), *icon);
}

bool UpdateAudioToggle(glm::vec2 mouse, int width, int height) {
    if (AudioToggleCovered() || !IsMouseButtonReleased(GLFW_MOUSE_BUTTON_LEFT) ||
        !Contains(GetAudioToggleRect(width, height), mouse))
        return false;
    SetGameAudioMuted(!gameAudio.muted);
    return true;
}

BoardLayout GetSelectionBoardLayout(int width, int height) {
    const float uiScale = SelectionUiScale(width, height);
    const float scale = std::clamp(std::min({ uiScale,
        (width - 24.0f * uiScale) / 762.5f,
        (height - 45.0f * uiScale - 91.0f * uiScale) / 460.0f }), 0.55f, 1.28f);
    // The SWF places the board at x=17, 1.75 px left of centering its
    // 762.5 px width on the 800 px stage.
    return { scale, (width - 762.5f * scale) * 0.5f - 1.75f * scale, BoardStageY() * uiScale };
}

// Horizontal stage offset of the selection strip's SWF placements.
constexpr float kSelectionStageOffsetX = 0.0f;

struct SelectionControls {
    float uiScale;
    float offset;
    float footerTop;
    Rect help;
    Rect easy;
    Rect normal;
    Rect hard;
    Rect mapLabel;
    Rect mapInput;
    Rect changeMap;
    Rect randomMap;
};

SelectionControls GetSelectionControls(int width, int height) {
    const float scale = SelectionUiScale(width, height);
    const float offset = SelectionUiOffset(width, scale);
    // These are the placements from splash-screen SWF frame 10,
    // including the root movie's -0.6 px horizontal offset.
    auto swf = [=](float x, float y, float w, float h) {
        return Rect(offset + (x + kSelectionStageOffsetX) * scale, y * scale, w * scale, h * scale);
    };
    auto swfButton = [=](float x) { return swf(x, 8.5f, 98.0f, 32.0f); };
    // mapLabel is the 251x31 outline (shape 197) that frames the label,
    // map field and CHANGE MAP; the field plate is shape 38 at (490, 8.05).
    return { scale, offset, height - 91.0f * scale,
        swfButton(17.1f), swfButton(127.95f),
        swfButton(224.95f), swfButton(321.95f),
        swf(431.5f, 5.95f, 251.0f, 31.0f),
        swf(490.0f, 8.05f, 98.0f, 32.0f), swfButton(586.0f),
        swfButton(689.05f) };
}

std::array<Rect, 6> GetSelectionButtonRects(const SelectionControls& controls) {
    return { controls.help, controls.easy, controls.normal, controls.hard,
        controls.changeMap, controls.randomMap };
}

void UpdateSelectionButtons(const SelectionControls& controls, glm::vec2 mouse) {
    const std::array<Rect, 6> rects = GetSelectionButtonRects(controls);
    const std::array<const char*, 6> labels = {
        "help", "easy", "normal", "hard", "CHANGE MAP", "RANDOM MAP"
    };
    for (size_t index = 0; index < rects.size(); ++index) {
        std::unique_ptr<Button>& button = game.selectionButtons[index];
        if (!button) {
            button = std::make_unique<Button>(rects[index], 12.0f * controls.uiScale,
                labels[index], Color(55, 55, 55));
        }
        button->rect = rects[index];
        button->Update(mouse);
    }
}

struct MapNumberTextPlacement {
    glm::vec2 pen;
    float rotation;
};

MapNumberTextPlacement GetMapNumberTextPlacement(const SelectionControls& controls,
    const TextInputBox& input) {
    // SWF edit-text field 198 (map_select): FairfaxStation 16 px, centered,
    // placed by (a,b,c,d,tx,ty)=(0.999542, 0.026794, -0.026794, 0.999542,
    // 492.3, 12.55). Its line is centered at x=45.5 with the first baseline
    // at y=14.188 in field space; Flash rasterizes it half a pixel higher.
    constexpr float fieldA = 0.999542f;
    constexpr float fieldB = 0.026794f;
    constexpr float fieldC = -0.026794f;
    constexpr float fieldD = 0.999542f;
    const float scale = controls.uiScale;
    const float lineWidth = input.renderText.MeasureText(input.text).maxWidth / scale;
    const float localX = 45.5f - lineWidth * 0.5f;
    const float localY = 13.688f;
    const float stageX = 492.3f + fieldA * localX + fieldC * localY;
    const float stageY = 12.55f + fieldB * localX + fieldD * localY;
    return { glm::vec2(controls.offset + (stageX + kSelectionStageOffsetX) * scale, stageY * scale),
        std::atan2(fieldB, fieldA) };
}

void DrawMapNumberText(const SelectionControls& controls) {
    TextInputBox& input = *game.mapNumberInput;
    if (input.text.empty()) return;
    const MapNumberTextPlacement placement = GetMapNumberTextPlacement(controls, input);
    input.renderText.setString(input.text);
    input.renderText.color = Color(0, 0, 0);
    input.renderText.DrawPrecise(placement.pen.x, placement.pen.y,
        TextAlignmentX::LEFT, TextAlignmentY::BASELINE, placement.rotation);

    const bool selectionShown = input.hasFocus && input.selectionStart != -1 &&
        input.selectionEnd != -1 && input.selectionStart != input.selectionEnd;
    if (!selectionShown) return;
    // Redraw the same string in white, clipped to the box's highlight (which
    // starts at the pen snapped down to a whole pixel), so the inverted
    // digits keep exactly the same glyph positions.
    const int start = std::clamp(std::min(input.selectionStart, input.selectionEnd), 0,
        static_cast<int>(input.text.size()));
    const int end = std::clamp(std::max(input.selectionStart, input.selectionEnd), 0,
        static_cast<int>(input.text.size()));
    const float lineX = std::floor(placement.pen.x);
    const float left = lineX + input.renderText.GetCursorX(input.text, start);
    const float right = lineX + input.renderText.GetCursorX(input.text, end);
    int framebufferWidth = 0, framebufferHeight = 0;
    glfwGetFramebufferSize(glfwGetCurrentContext(), &framebufferWidth, &framebufferHeight);
    const float toFramebufferX = framebufferWidth / static_cast<float>(std::max(1, GetScreenWidth()));
    const float toFramebufferY = framebufferHeight / static_cast<float>(std::max(1, GetScreenHeight()));
    const Rect box = input.rect;
    glEnable(GL_SCISSOR_TEST);
    glScissor(static_cast<GLint>(std::lround(left * toFramebufferX)),
        static_cast<GLint>(std::lround(framebufferHeight - (box.position.y + box.size.y) * toFramebufferY)),
        static_cast<GLsizei>(std::max(0L, std::lround((right - left) * toFramebufferX))),
        static_cast<GLsizei>(std::lround(box.size.y * toFramebufferY)));
    input.renderText.setString(input.text);
    input.renderText.color = Color(255, 255, 255);
    input.renderText.DrawPrecise(placement.pen.x, placement.pen.y,
        TextAlignmentX::LEFT, TextAlignmentY::BASELINE, placement.rotation);
    glDisable(GL_SCISSOR_TEST);
}

void ConfigureMapNumberInput(const SelectionControls& controls) {
    if (!game.mapNumberInput) {
        game.mapNumberInput = std::make_unique<TextInputBox>(
            controls.mapInput.position, controls.mapInput.size, 13.0f * controls.uiScale, "");
        game.mapNumberInput->text = MapSeedText();
        game.mapNumberInput->finalText = game.mapNumberInput->text;
        game.mapNumberInput->cursorPosition = static_cast<int>(game.mapNumberInput->text.size());
    }

    TextInputBox& input = *game.mapNumberInput;
    input.rect = controls.mapInput;
    input.renderText.setCharacterSize(16.0f * controls.uiScale);
    // A slight thinning matches the Flash player's lighter antialiasing.
    input.renderText.setFontWeight(350);
    input.renderText.setHorizontalScale(1.0f);
    if (artwork.mapInputFont)
        input.renderText.setFont(*artwork.mapInputFont);
    input.renderText.setString(input.text);
    input.offsetFromLeft = 5;
    // Keep the box's cursor and selection on the same line start as the
    // rotated text drawn by DrawMapNumberText.
    const MapNumberTextPlacement placement = GetMapNumberTextPlacement(controls, input);
    input.textOffsetX = placement.pen.x - controls.mapInput.position.x - input.offsetFromLeft;
    input.textOffsetY = -6.0f * controls.uiScale;
    input.color = glm::vec4(0.0f);
    input.textColor = Color(0, 0, 0);
    input.selectionColor = Color(26, 28, 32, 245);
    input.selectedTextColor = Color(255, 255, 255);
    input.invertSelection = true;
}

struct HelpControls {
    Rect done;
    Rect middle;
    Rect next;
};

HelpControls GetHelpControls(int width, int height) {
    // The authored help movie is a 900x600 stage. Center it just like the
    // ActionScript dialog, whose visible parchment frame is narrower than its stage.
    const float offsetX = (width - 900.0f) * 0.5f;
    const float offsetY = (height - 600.0f) * 0.5f;
    auto control = [=](float x) {
        return Rect(offsetX + x, offsetY + 534.0f, 98.0f, 32.0f);
    };
    return { control(190.0f), control(289.0f), control(388.0f) };
}

enum class ButtonVisualState { Normal, Hovered, Pressed, Inactive };

ButtonVisualState SelectionButtonVisualState(size_t index, bool selected = false) {
    if (selected) return ButtonVisualState::Inactive;
    const Button* button = game.selectionButtons[index].get();
    if (!button) return ButtonVisualState::Normal;
    switch (button->state) {
    case Button::States::HELD:
    case Button::States::WAS_HELD:
        return ButtonVisualState::Pressed;
    case Button::States::HOVERING:
    case Button::States::PRESSED:
        return ButtonVisualState::Hovered;
    default:
        return ButtonVisualState::Normal;
    }
}

void DrawPillButton(Rect hitRect, const std::string& label, bool red,
    ButtonVisualState state, float scale, glm::vec4 labelColor, int idleOpacity = 255,
    float artworkHeight = 0.0f, Texture* colorOverlay = nullptr,
    int colorOverlayOpacity = 0) {
    const bool inactive = state == ButtonVisualState::Inactive;
    // The SWF's inactive frame always switches to the silver plate, regardless
    // of the button's configured red or white style.
    Texture* button = red && !inactive
        ? artwork.redButton.get() : artwork.silverButton.get();
    if (!button) return;

    glm::vec2 center = hitRect.GetCenter();
    const bool hovered = state == ButtonVisualState::Hovered;
    const bool pressed = state == ButtonVisualState::Pressed;
    const float renderedHeight = artworkHeight > 0.0f
        ? artworkHeight * scale : hitRect.size.y;
    Rect artworkRect(center.x - hitRect.size.x * 0.5f,
        center.y - renderedHeight * 0.5f, hitRect.size.x, renderedHeight);
    const int stateAlpha = inactive ? 128 : std::clamp(idleOpacity, 0, 255);
    const glm::vec4 buttonTint = Color(255, 255, 255, stateAlpha);
    DrawRectangle(artworkRect, buttonTint, *button);
    if (colorOverlay && colorOverlayOpacity > 0)
        DrawRectangle(artworkRect,
            Color(255, 255, 255, std::clamp(colorOverlayOpacity, 0, 255)), *colorOverlay);
    if (hovered) {
        Texture* hoverMask = red ? artwork.redButtonHover.get() : artwork.silverButtonHover.get();
        if (hoverMask)
            DrawRectangle(artworkRect, Color(255, 255, 255, 77), *hoverMask);
    } else if (pressed) {
        DrawRectangle(artworkRect, Color(0, 0, 0, 51), *button);
    }

    if (!label.empty()) {
        Text* renderer = extras::defaultTextRenderer;
        Font* previousFont = renderer ? renderer->font : nullptr;
        const int previousWeight = renderer ? renderer->fontWeight : 400;
        const float previousHorizontalScale = renderer ? renderer->horizontalScale : 1.0f;
        if (artwork.arialNarrowFont && renderer)
            renderer->setFont(*artwork.arialNarrowFont);
        // SWF edit-text field 36 is Arial Narrow at 12.5 px, transformed by
        // (a,b,c,d)=(1.09549, 0.01910, -0.01723, 0.98671). The extracted
        // font already contains the source glyph outlines, so don't add a
        // second synthetic bold pass.
        constexpr float fieldScaleX = 1.095489501953125f;
        constexpr float fieldScaleY = 0.9867095947265625f;
        constexpr float fieldShearX = -0.0172271728515625f;
        constexpr float fieldShearY = 0.01910400390625f;
        const float labelFontSize = 12.5f * std::hypot(fieldShearX, fieldScaleY) * scale;
        const float labelHorizontalScale = std::hypot(fieldScaleX, fieldShearY) /
            std::hypot(fieldShearX, fieldScaleY);
        const float labelRotation = std::atan2(fieldShearY, fieldScaleX);
        // The all-caps map actions need a small raster-weight lift at this
        // size to match the Flash player's antialiasing.
        const int labelWeight = (label == "CHANGE MAP" || label == "RANDOM MAP")
            ? 500 : 400;
        if (renderer) {
            renderer->setFontWeight(labelWeight);
            renderer->setHorizontalScale(labelHorizontalScale);
            renderer->setString(label);
            renderer->setCharacterSize(labelFontSize);
            renderer->setColor(inactive ? Color(255, 255, 255, 128) : labelColor);
        }
        // Transform the SWF field's centered baseline through its matrix.
        const glm::vec2 labelBaseline(
            hitRect.position.x + 46.8354f * scale,
            hitRect.position.y + 17.3026f * scale);
        if (renderer)
            renderer->DrawPrecise(labelBaseline.x, labelBaseline.y,
                TextAlignmentX::CENTER, TextAlignmentY::BASELINE, labelRotation);
        if (renderer)
            renderer->setHorizontalScale(previousHorizontalScale);
        if (renderer)
            renderer->setFontWeight(previousWeight);
        if (previousFont && renderer)
            renderer->setFont(*previousFont);
    }
}

void DrawTerrainPatchesClipped(const BoardLayout& layout, bool dry = false) {
    // The source keeps its tiled land background underneath the transparent
    // sea masks. Restricting it to board cells preserves the map silhouette.
    // dry paints the war-torn background_1 copy from ld_*.png instead.
    if (game.blankMapNumber) {
        // Both backgrounds stay empty, so the stage's shape 542 shows.
        if (!dry) DrawHexFillBatch(layout, Color(154, 200, 78), [](int) { return true; });
        return;
    }
    DrawHexFillBatch(layout, dry ? Color(147, 93, 52) : Color(126, 157, 72), [](int) { return true; });

    glEnable(GL_STENCIL_TEST);
    glStencilMask(0xFF);
    glClearStencil(0);
    glClear(GL_STENCIL_BUFFER_BIT);
    glStencilFunc(GL_ALWAYS, 1, 0xFF);
    glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    DrawHexFillBatch(layout, Color(255, 255, 255), [](int) { return true; });

    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glStencilMask(0x00);
    glStencilFunc(GL_EQUAL, 1, 0xFF);
    glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
    for (int patchX = 0; patchX < 6; ++patchX) {
        for (int patchY = 0; patchY < 4; ++patchY) {
            const TerrainPatch& patch = game.terrainPatches[static_cast<size_t>(patchY * 6 + patchX)];
            Texture* land = dry ? PickTexture(artwork.dryLand, patch.dryLandVariant)
                : PickTexture(artwork.land, patch.landVariant);
            if (!land) continue;
            const Rect bounds(layout.left + (patchX * 125.0f - 15.0f) * layout.scale,
                layout.top + (patchY * 125.0f - 15.0f) * layout.scale,
                land->width * layout.scale, land->height * layout.scale);
            glm::vec4 source(patch.flipHorizontal ? static_cast<float>(land->width) : 0.0f,
                patch.flipVertical ? static_cast<float>(land->height) : 0.0f,
                patch.flipHorizontal ? -static_cast<float>(land->width) : static_cast<float>(land->width),
                patch.flipVertical ? -static_cast<float>(land->height) : static_cast<float>(land->height));
            DrawRectangle(bounds, glm::radians(static_cast<float>(patch.rotation)), *land, &source);
        }
    }
    glStencilMask(0xFF);
    glDisable(GL_STENCIL_TEST);
}

void DrawTownTerrainClipped(const BoardLayout& layout, int width, int height, bool dry = false) {
    // c_NaN.png and cd_NaN.png never load.
    if (game.blankMapNumber) return;
    const float uiScale = SelectionUiScale(width, height);
    const float stageOffset = SelectionUiOffset(width, uiScale);
    glEnable(GL_STENCIL_TEST);
    glStencilMask(0xFF);
    glClearStencil(0);
    glClear(GL_STENCIL_BUFFER_BIT);
    glStencilFunc(GL_ALWAYS, 1, 0xFF);
    glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    DrawHexFillBatch(layout, Color(255, 255, 255), [](int) { return true; });

    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glStencilMask(0x00);
    glStencilFunc(GL_EQUAL, 1, 0xFF);
    glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
    for (int x = 0; x < kMapWidth; ++x) {
        for (int y = 0; y < kMapHeight; ++y) {
            const int index = Index(x, y);
            const Tile& tile = game.tiles[index];
            if (tile.estate != Estate::Town && tile.estate != Estate::Port) continue;
            const bool dryTown = dry || tile.dry;
            TextureSet& textureSet = dryTown ? artwork.dryTowns : artwork.towns;
            Texture* town = PickTexture(textureSet,
                dryTown ? tile.dryArtVariant : tile.artVariant);
            if (!town) continue;
            const glm::vec2 center = TileCenter(index, layout);
            const float size = town->width * layout.scale;
            // The SWF copies town sprites into its map bitmap at integer pixel
            // coordinates, then scales the map for the UI. Snap in that bitmap
            // coordinate space; snapping screen coordinates would vary with
            // the window's centering offset and blur alternating hex columns.
            const glm::vec2 stageCenter((center.x - stageOffset) / uiScale,
                center.y / uiScale);
            const float stageSize = size / uiScale;
            const float stageLeft = static_cast<float>(
                static_cast<int>(stageCenter.x - stageSize * 0.5f));
            const float stageTop = static_cast<float>(
                static_cast<int>(stageCenter.y - stageSize * 0.5f));
            const Rect bounds(stageOffset + stageLeft * uiScale,
                stageTop * uiScale, size, size);
            glm::vec4 source(tile.artFlipHorizontal ? static_cast<float>(town->width) : 0.0f,
                tile.artFlipVertical ? static_cast<float>(town->height) : 0.0f,
                tile.artFlipHorizontal ? -static_cast<float>(town->width) : static_cast<float>(town->width),
                tile.artFlipVertical ? -static_cast<float>(town->height) : static_cast<float>(town->height));
            DrawRectangle(bounds, glm::radians(static_cast<float>(tile.artRotation)), *town, &source);
        }
    }
    glStencilMask(0xFF);
    glDisable(GL_STENCIL_TEST);
}

struct BoardOutlinePoint {
    int x = 0;
    int y = 0;
};

bool SameBoardOutlinePoint(const BoardOutlinePoint& left, const BoardOutlinePoint& right) {
    return left.x == right.x && left.y == right.y;
}

std::int64_t BoardOutlineCross(const BoardOutlinePoint& a,
    const BoardOutlinePoint& b, const BoardOutlinePoint& c) {
    return static_cast<std::int64_t>(b.x - a.x) * (c.y - a.y) -
        static_cast<std::int64_t>(b.y - a.y) * (c.x - a.x);
}

const std::vector<float>& GetBoardOutlineTriangles(const BoardLayout& layout) {
    struct BoundaryEdge {
        BoardOutlinePoint start;
        BoardOutlinePoint end;
    };
    static std::vector<float> vertices;
    static BoardLayout cachedLayout{};
    static bool hasCachedLayout = false;
    if (hasCachedLayout && cachedLayout.scale == layout.scale &&
        cachedLayout.left == layout.left && cachedLayout.top == layout.top)
        return vertices;

    // Work in half-pixel map units so adjacent hexes share exactly matching
    // vertices before the complete outside contour is triangulated.
    static constexpr std::array<BoardOutlinePoint, 6> cornerOffsets = {{
        {-50, 0}, {-25, -40}, {25, -40}, {50, 0}, {25, 40}, {-25, 40}
    }};
    std::vector<BoundaryEdge> edges;
    edges.reserve(kTileCount * 2);
    for (int index = 0; index < kTileCount; ++index) {
        const int x = TileX(index);
        const int y = TileY(index);
        const BoardOutlinePoint center{ 50 + x * 75, 40 + y * 80 + (x % 2) * 40 };
        std::array<BoardOutlinePoint, 6> corners{};
        for (size_t corner = 0; corner < corners.size(); ++corner) {
            corners[corner] = {
                center.x + cornerOffsets[corner].x,
                center.y + cornerOffsets[corner].y
            };
        }

        for (size_t side = 0; side < corners.size(); ++side) {
            const BoundaryEdge candidate{ corners[side], corners[(side + 1) % corners.size()] };
            const auto shared = std::find_if(edges.begin(), edges.end(), [&](const BoundaryEdge& edge) {
                return SameBoardOutlinePoint(edge.start, candidate.end) &&
                    SameBoardOutlinePoint(edge.end, candidate.start);
            });
            if (shared != edges.end()) edges.erase(shared);
            else edges.push_back(candidate);
        }
    }

    std::vector<BoardOutlinePoint> outline;
    std::vector<bool> used(edges.size(), false);
    if (!edges.empty()) {
        outline.reserve(edges.size());
        outline.push_back(edges.front().start);
        used.front() = true;
        BoardOutlinePoint current = edges.front().end;
        while (!SameBoardOutlinePoint(current, outline.front()) && outline.size() <= edges.size()) {
            outline.push_back(current);
            size_t nextEdge = edges.size();
            for (size_t edge = 0; edge < edges.size(); ++edge) {
                if (!used[edge] && SameBoardOutlinePoint(edges[edge].start, current)) {
                    nextEdge = edge;
                    break;
                }
            }
            if (nextEdge == edges.size()) {
                outline.clear();
                break;
            }
            used[nextEdge] = true;
            current = edges[nextEdge].end;
        }
    }

    // Remove collinear contour points before ear clipping the single concave
    // map polygon. This avoids internal per-hex edges in the water mask.
    bool removedCollinearPoint = true;
    while (removedCollinearPoint && outline.size() > 3) {
        removedCollinearPoint = false;
        for (size_t point = 0; point < outline.size(); ++point) {
            const size_t previous = (point + outline.size() - 1) % outline.size();
            const size_t next = (point + 1) % outline.size();
            if (BoardOutlineCross(outline[previous], outline[point], outline[next]) == 0) {
                outline.erase(outline.begin() + static_cast<std::ptrdiff_t>(point));
                removedCollinearPoint = true;
                break;
            }
        }
    }

    vertices.clear();
    if (outline.size() >= 3) {
        std::int64_t signedArea = 0;
        for (size_t point = 0; point < outline.size(); ++point) {
            const BoardOutlinePoint& current = outline[point];
            const BoardOutlinePoint& next = outline[(point + 1) % outline.size()];
            signedArea += static_cast<std::int64_t>(current.x) * next.y -
                static_cast<std::int64_t>(next.x) * current.y;
        }
        const std::int64_t orientation = signedArea >= 0 ? 1 : -1;
        std::vector<size_t> remaining(outline.size());
        for (size_t point = 0; point < remaining.size(); ++point) remaining[point] = point;

        const auto appendPoint = [&](const BoardOutlinePoint& point) {
            vertices.push_back(layout.left + point.x * 0.5f * layout.scale);
            vertices.push_back(layout.top + point.y * 0.5f * layout.scale);
            vertices.push_back(0.0f);
            vertices.push_back(0.0f);
            vertices.push_back(0.0f);
        };
        const auto pointInTriangle = [&](const BoardOutlinePoint& point,
            const BoardOutlinePoint& a, const BoardOutlinePoint& b,
            const BoardOutlinePoint& c) {
            return orientation * BoardOutlineCross(a, b, point) >= 0 &&
                orientation * BoardOutlineCross(b, c, point) >= 0 &&
                orientation * BoardOutlineCross(c, a, point) >= 0;
        };

        bool triangulationSucceeded = true;
        while (remaining.size() > 3) {
            bool clippedEar = false;
            for (size_t position = 0; position < remaining.size(); ++position) {
                const size_t previousPosition = (position + remaining.size() - 1) % remaining.size();
                const size_t nextPosition = (position + 1) % remaining.size();
                const size_t previous = remaining[previousPosition];
                const size_t current = remaining[position];
                const size_t next = remaining[nextPosition];
                if (orientation * BoardOutlineCross(outline[previous], outline[current], outline[next]) <= 0)
                    continue;

                bool containsAnotherPoint = false;
                for (size_t candidate : remaining) {
                    if (candidate == previous || candidate == current || candidate == next) continue;
                    if (pointInTriangle(outline[candidate], outline[previous], outline[current], outline[next])) {
                        containsAnotherPoint = true;
                        break;
                    }
                }
                if (containsAnotherPoint) continue;

                appendPoint(outline[previous]);
                appendPoint(outline[current]);
                appendPoint(outline[next]);
                remaining.erase(remaining.begin() + static_cast<std::ptrdiff_t>(position));
                clippedEar = true;
                break;
            }
            if (!clippedEar) {
                triangulationSucceeded = false;
                break;
            }
        }
        if (triangulationSucceeded && remaining.size() == 3) {
            appendPoint(outline[remaining[0]]);
            appendPoint(outline[remaining[1]]);
            appendPoint(outline[remaining[2]]);
        } else {
            vertices.clear();
        }
    }

    cachedLayout = layout;
    hasCachedLayout = true;
    return vertices;
}

void BeginBoardStencil(const BoardLayout& layout) {
    glEnable(GL_STENCIL_TEST);
    glStencilMask(0xFF);
    glClearStencil(0);
    glClear(GL_STENCIL_BUFFER_BIT);
    glStencilFunc(GL_ALWAYS, 1, 0xFF);
    glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    DrawSolidTriangles(GetBoardOutlineTriangles(layout), Color(255, 255, 255));
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glStencilMask(0x00);
    glStencilFunc(GL_EQUAL, 1, 0xFF);
    glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
}

void EndBoardStencil() {
    glStencilMask(0xFF);
    glDisable(GL_STENCIL_TEST);
}

constexpr int kOceanStageWidth = 800;
constexpr int kOceanStageHeight = 600;
constexpr size_t kOceanStagePixelCount =
    static_cast<size_t>(kOceanStageWidth) * kOceanStageHeight;
std::vector<unsigned char> BuildSeaAndPortBitmap(const BoardLayout& layout) {
    static constexpr int portVariants[6] = { 2, 1, 2, 2, 1, 2 };
    static constexpr bool portFlipX[6] = { true, false, false, false, false, true };
    static constexpr bool portFlipY[6] = { true, true, true, false, false, false };

    // The SWF uses BitmapData.copyPixels(..., mergeAlpha=true) to assemble the
    // sea image. Build the same straight-alpha bitmap in CPU memory so the
    // source pixel coverage does not depend on GPU stamp filtering.
    std::vector<unsigned char> bitmap(kOceanStagePixelCount * 4, 0);
    const auto pasteBitmap = [&](float centerX, float centerY, Texture& source,
        bool flipHorizontal, bool flipVertical, int rotation) {
        if (source.width <= 0 || source.height <= 0) return;
        const size_t sourceBytes = static_cast<size_t>(source.width) * source.height * 4;
        if (source.textureData.size() < sourceBytes) return;

        const int destinationWidth = std::max(1,
            static_cast<int>(std::lround(source.width * layout.scale)));
        const int destinationHeight = std::max(1,
            static_cast<int>(std::lround(source.height * layout.scale)));
        const float scaleX = static_cast<float>(destinationWidth) / source.width;
        const float scaleY = static_cast<float>(destinationHeight) / source.height;
        const int left = static_cast<int>(centerX - source.width * layout.scale * 0.5f);
        const int top = static_cast<int>(centerY - source.height * layout.scale * 0.5f);
        const bool rotate180 = ((rotation % 360) + 360) % 360 == 180;

        for (int destinationY = 0; destinationY < destinationHeight; ++destinationY) {
            const int screenY = top + destinationY;
            if (screenY < 0 || screenY >= kOceanStageHeight) continue;
            int sourceY = std::clamp(static_cast<int>((destinationY + 0.5f) / scaleY),
                0, source.height - 1);
            if (rotate180) sourceY = source.height - sourceY - 1;
            if (flipVertical) sourceY = source.height - sourceY - 1;
            // Texture::textureData is stored bottom-up after image loading.
            const int sourceDataY = source.height - sourceY - 1;
            const size_t destinationRow =
                static_cast<size_t>(kOceanStageHeight - screenY - 1) * kOceanStageWidth;
            for (int destinationX = 0; destinationX < destinationWidth; ++destinationX) {
                const int screenX = left + destinationX;
                if (screenX < 0 || screenX >= kOceanStageWidth) continue;
                int sourceX = std::clamp(static_cast<int>((destinationX + 0.5f) / scaleX),
                    0, source.width - 1);
                if (rotate180) sourceX = source.width - sourceX - 1;
                if (flipHorizontal) sourceX = source.width - sourceX - 1;

                const size_t sourcePixel =
                    (static_cast<size_t>(sourceDataY) * source.width + sourceX) * 4;
                const size_t destinationPixel = (destinationRow + screenX) * 4;
                const unsigned int sourceAlpha = source.textureData[sourcePixel + 3];
                if (sourceAlpha == 0) continue;
                if (sourceAlpha == 255) {
                    std::copy_n(source.textureData.data() + sourcePixel, 4,
                        bitmap.data() + destinationPixel);
                    continue;
                }

                const unsigned int destinationAlpha = bitmap[destinationPixel + 3];
                const unsigned int inverseSourceAlpha = 255 - sourceAlpha;
                const unsigned int outputAlphaNumerator =
                    sourceAlpha * 255 + destinationAlpha * inverseSourceAlpha;
                if (outputAlphaNumerator == 0) continue;
                const unsigned int outputAlpha = (outputAlphaNumerator + 127) / 255;
                for (int channel = 0; channel < 3; ++channel) {
                    const unsigned int sourceContribution =
                        static_cast<unsigned int>(source.textureData[sourcePixel + channel]) *
                        sourceAlpha * 255;
                    const unsigned int destinationContribution =
                        static_cast<unsigned int>(bitmap[destinationPixel + channel]) *
                        destinationAlpha * inverseSourceAlpha;
                    bitmap[destinationPixel + channel] = static_cast<unsigned char>(
                        std::clamp((sourceContribution + destinationContribution +
                            outputAlphaNumerator / 2) / outputAlphaNumerator, 0U, 255U));
                }
                bitmap[destinationPixel + 3] = static_cast<unsigned char>(outputAlpha);
            }
        }
    };

    // Match the SWF's x-major copyPixels order, including port stamps directly
    // after the sea tile that owns them.
    for (int x = 0; x < kMapWidth; ++x) {
        for (int y = 0; y < kMapHeight; ++y) {
            const int index = Index(x, y);
            const Tile& tile = game.tiles[index];
            if (IsLandForShoreline(tile)) continue;
            const glm::vec2 center = TileCenter(index, layout);

            if (Texture* sea = PickTexture(artwork.sea, tile.artVariant)) {
                // The SWF's shoreline matches when its H/V flip flags are
                // mapped to the opposite source axes in the CPU bitmap stamp.
                pasteBitmap(center.x, center.y,
                    *sea, tile.artFlipVertical, tile.artFlipHorizontal, tile.artRotation);
            }

            for (int direction = 0; direction < 6; ++direction) {
                const int neighbour = NeighbourInDirection(index, direction);
                if (neighbour < 0 || game.tiles[neighbour].estate != Estate::Port) continue;
                Texture* port = artwork.seaPorts[static_cast<size_t>(portVariants[direction] - 1)].get();
                if (port)
                    pasteBitmap(center.x, center.y,
                        *port, portFlipX[direction], portFlipY[direction], 0);
            }
        }
    }
    return bitmap;
}

void ApplyFlashSeaGlowFilters(std::vector<unsigned char>& bitmap) {
    if (bitmap.size() < kOceanStagePixelCount * 4) return;

    struct GlowFilter {
        unsigned char red;
        unsigned char green;
        unsigned char blue;
        float blur;
        float strength;
        bool inner;
    };
    // These are the four GlowFilters attached to the SWF's sea bitmap clip.
    // The pale yellow and olive filters create the narrow sand-colored coast;
    // the large blue inner glow gives the water its soft edge.
    constexpr std::array<GlowFilter, 4> filters = {{
        { 0x50, 0xAA, 0xED, 75.0f, 0.59765625f, true },
        { 0xFF, 0xFC, 0x84, 3.0f, 2.5f, true },
        { 0x55, 0x4E, 0x24, 0.0f, 4.0f, false },
        { 0xC8, 0xCA, 0x62, 8.0f, 0.40625f, false }
    }};

    const auto blurAlpha = [](const std::vector<float>& source, float blurSize) {
        if (blurSize <= 0.0f) return source;
        constexpr int width = kOceanStageWidth;
        constexpr int height = kOceanStageHeight;
        // The SWF encodes one blur pass for these filters. Integrate each
        // pixel's fractional overlap with the centered box window so odd and
        // even blur widths both retain Flash's sub-pixel edge coverage.
        const auto integralAt = [](const std::vector<float>& prefix,
            const std::vector<float>& values, int length, float edge) {
            edge = std::clamp(edge, 0.0f, static_cast<float>(length));
            const int whole = std::min(length, static_cast<int>(std::floor(edge)));
            float result = prefix[whole];
            if (whole < length)
                result += values[whole] * (edge - whole);
            return result;
        };

        std::vector<float> horizontal(source.size(), 0.0f);
        std::vector<float> blurred(source.size(), 0.0f);
        std::vector<float> values(std::max(width, height), 0.0f);
        std::vector<float> prefix(values.size() + 1, 0.0f);
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x)
                values[x] = source[static_cast<size_t>(y) * width + x];
            prefix[0] = 0.0f;
            for (int x = 0; x < width; ++x)
                prefix[x + 1] = prefix[x] + values[x];
            for (int x = 0; x < width; ++x) {
                const float left = x + 0.5f - blurSize * 0.5f;
                const float right = left + blurSize;
                const float sum = integralAt(prefix, values, width, right) -
                    integralAt(prefix, values, width, left);
                horizontal[static_cast<size_t>(y) * width + x] = sum / blurSize;
            }
        }
        for (int x = 0; x < width; ++x) {
            for (int y = 0; y < height; ++y)
                values[y] = horizontal[static_cast<size_t>(y) * width + x];
            prefix[0] = 0.0f;
            for (int y = 0; y < height; ++y)
                prefix[y + 1] = prefix[y] + values[y];
            for (int y = 0; y < height; ++y) {
                const float top = y + 0.5f - blurSize * 0.5f;
                const float bottom = top + blurSize;
                const float sum = integralAt(prefix, values, height, bottom) -
                    integralAt(prefix, values, height, top);
                blurred[static_cast<size_t>(y) * width + x] = sum / blurSize;
            }
        }
        return blurred;
    };

    std::vector<float> filterSourceAlpha(kOceanStagePixelCount, 0.0f);
    for (const GlowFilter& filter : filters) {
        // Flash applies the sea clip's filters as an ordered stack. Snapshot the
        // alpha entering this pass so later filters see earlier outer-glow
        // coverage, while pixels within this pass still use a stable mask.
        for (size_t pixel = 0; pixel < kOceanStagePixelCount; ++pixel)
            filterSourceAlpha[pixel] = bitmap[pixel * 4 + 3] / 255.0f;
        const std::vector<float> blurred = blurAlpha(filterSourceAlpha, filter.blur);
        for (size_t pixel = 0; pixel < kOceanStagePixelCount; ++pixel) {
            const float destinationAlpha = bitmap[pixel * 4 + 3] / 255.0f;
            const float glowCoverage = filter.inner
                ? (1.0f - blurred[pixel])
                : blurred[pixel];
            const float glowAlpha = std::clamp(glowCoverage * filter.strength,
                0.0f, 1.0f);
            if (glowAlpha <= 0.0f) continue;

            const size_t rgba = pixel * 4;
            // Inner glow recolors within the current image alpha. Outer glow
            // is composited behind the current image, adding coverage only in
            // the transparent portion of the blurred edge. Keep the inner
            // filter strength independent of the source pixel alpha; source
            // coverage is applied once by the inner-glow composite below.
            const float sourceContributionAlpha = filter.inner
                ? destinationAlpha * (1.0f - glowAlpha)
                : destinationAlpha;
            const float glowContributionAlpha = filter.inner
                ? destinationAlpha * glowAlpha
                : glowAlpha * (1.0f - destinationAlpha);
            const float outputAlpha = sourceContributionAlpha + glowContributionAlpha;
            if (outputAlpha <= 0.0f) continue;
            for (int channel = 0; channel < 3; ++channel) {
                const unsigned char glowColor = channel == 0 ? filter.red
                    : channel == 1 ? filter.green : filter.blue;
                const float sourceContribution =
                    bitmap[rgba + channel] * sourceContributionAlpha;
                const float glowContribution = glowColor * glowContributionAlpha;
                bitmap[rgba + channel] = static_cast<unsigned char>(std::clamp(
                    std::lround((sourceContribution + glowContribution) / outputAlpha),
                    0L, 255L));
            }
            bitmap[rgba + 3] = static_cast<unsigned char>(std::clamp(
                std::lround(outputAlpha * 255.0f), 0L, 255L));
        }
    }
}

struct OceanCompositeCache {
    std::unique_ptr<Texture> bitmap;
    std::uint64_t key = 0;
    bool ready = false;
};

std::uint64_t GetOceanCompositeKey(const BoardLayout& layout) {
    std::uint64_t key = 1469598103934665603ULL;
    const auto mix = [&](std::uint64_t value) {
        key = (key ^ value) * 1099511628211ULL;
    };
    for (const Tile& tile : game.tiles) {
        mix(IsLandForShoreline(tile) ? 1ULL : 0ULL);
        mix(static_cast<std::uint64_t>(tile.artVariant + 2));
        mix(static_cast<std::uint64_t>(tile.artRotation + 360));
        mix(tile.artFlipHorizontal ? 1ULL : 0ULL);
        mix(tile.artFlipVertical ? 1ULL : 0ULL);
        mix(tile.estate == Estate::Port ? 1ULL : 0ULL);
        mix(tile.land && tile.owner >= 0
            ? static_cast<std::uint64_t>(tile.owner + 1) : 0ULL);
    }
    mix(static_cast<std::uint64_t>(std::lround(layout.scale * 10000.0f)));
    mix(static_cast<std::uint64_t>(std::lround(layout.left * 1000.0f)));
    mix(static_cast<std::uint64_t>(std::lround(layout.top * 1000.0f)));
    return key;
}

// The board clip moves as one piece through sprite 232's slides, so its
// stage-sized bitmaps are built for this board y and drawn offset from it.
constexpr float kBoardBitmapStageTop = 117.0f;

void DrawOceanComposite(const BoardLayout& layout, int width, int height) {
    static OceanCompositeCache cache;
    const float uiScale = SelectionUiScale(width, height);
    const float stageOffset = SelectionUiOffset(width, uiScale);
    const float boardShift = layout.top / uiScale - kBoardBitmapStageTop;
    const BoardLayout stageLayout{
        layout.scale / uiScale,
        (layout.left - stageOffset) / uiScale,
        kBoardBitmapStageTop
    };
    const std::uint64_t key = GetOceanCompositeKey(stageLayout);

    if (!cache.ready || cache.key != key) {
        std::vector<unsigned char> bitmap = BuildSeaAndPortBitmap(stageLayout);
        ApplyFlashSeaGlowFilters(bitmap);
        cache.bitmap = std::make_unique<Texture>(
            kOceanStageWidth, kOceanStageHeight, std::move(bitmap));
        cache.bitmap->updateSamplerDetails(GL_FilterModes::NEAREST, GL_WrapModes::CLAMP_TO_EDGE);
        cache.key = key;
        cache.ready = true;
    }

    // The CPU bitmap is straight-alpha, like the SWF's copyPixels result.
    if (cache.ready && cache.bitmap) {
        const Rect stageRect(stageOffset, boardShift * uiScale,
            kOceanStageWidth * uiScale, kOceanStageHeight * uiScale);
        GLint previousBlendSrcRgb = GL_SRC_ALPHA;
        GLint previousBlendDstRgb = GL_ONE_MINUS_SRC_ALPHA;
        GLint previousBlendSrcAlpha = GL_ONE;
        GLint previousBlendDstAlpha = GL_ONE_MINUS_SRC_ALPHA;
        const GLboolean previousBlendEnabled = glIsEnabled(GL_BLEND);
        glGetIntegerv(GL_BLEND_SRC_RGB, &previousBlendSrcRgb);
        glGetIntegerv(GL_BLEND_DST_RGB, &previousBlendDstRgb);
        glGetIntegerv(GL_BLEND_SRC_ALPHA, &previousBlendSrcAlpha);
        glGetIntegerv(GL_BLEND_DST_ALPHA, &previousBlendDstAlpha);
        glEnable(GL_BLEND);
        glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA,
            GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        // Sea tiles carry wide outer-glow pixels. The SWF's map mask clips
        // that glow to the board silhouette so edge water cannot spill into
        // the surrounding backdrop.
        BeginBoardStencil(layout);
        DrawRectangle(stageRect, *cache.bitmap);
        EndBoardStencil();
        glBlendFuncSeparate(previousBlendSrcRgb, previousBlendDstRgb,
            previousBlendSrcAlpha, previousBlendDstAlpha);
        if (previousBlendEnabled) glEnable(GL_BLEND); else glDisable(GL_BLEND);
    }
}

void DrawFactionTerritoryGlow(const BoardLayout& layout, int width, int height) {
    constexpr int stageWidth = 800;
    constexpr int stageHeight = 600;
    static std::unique_ptr<Texture> cachedGlow;
    struct FactionMaskCache {
        std::uint64_t key = 0;
        std::vector<float> mask;
        std::vector<float> blurred;
    };
    static std::array<FactionMaskCache, kFactionCount> factionMasks;
    static std::uint64_t cachedOwnerHash = 0;
    static int cachedScaleKey = 0;

    const float uiScale = SelectionUiScale(width, height);
    const float stageOffset = SelectionUiOffset(width, uiScale);
    const float boardShift = layout.top / uiScale - kBoardBitmapStageTop;
    const BoardLayout localLayout{
        layout.scale / uiScale,
        (layout.left - stageOffset) / uiScale,
        kBoardBitmapStageTop
    };

    auto coverage = [](int index, int faction) {
        if (game.screen != Screen::Playing)
            return game.tiles[index].land && game.tiles[index].owner == faction ? 1.0f : 0.0f;
        return game.territoryAlpha[index][faction];
    };
    std::uint64_t ownerHash = 1469598103934665603ULL;
    for (int index = 0; index < kTileCount; ++index)
        for (int faction = 0; faction < kFactionCount; ++faction)
            ownerHash = (ownerHash ^ static_cast<std::uint64_t>(std::lround(coverage(index, faction) * 255.0f))) * 1099511628211ULL;
    const int scaleKey = static_cast<int>(std::lround(localLayout.scale * 10000.0f));
    const int layoutLeftKey = static_cast<int>(std::lround(localLayout.left * 10000.0f));
    const int layoutTopKey = static_cast<int>(std::lround(localLayout.top * 10000.0f));
    static int cachedLayoutLeftKey = 0;
    static int cachedLayoutTopKey = 0;

    if (!cachedGlow || cachedOwnerHash != ownerHash || cachedScaleKey != scaleKey ||
        cachedLayoutLeftKey != layoutLeftKey || cachedLayoutTopKey != layoutTopKey) {
        const size_t pixelCount = static_cast<size_t>(stageWidth) * stageHeight;
        std::vector<unsigned char> rgba(pixelCount * 4, 0);

        // GlowFilter quality 1 is a single box blur. For blurX = blurY = 30 the
        // window covers x - 15 .. x + 14, which matches the SWF render best.
        auto blurMask = [=](std::vector<float>& mask, int boxWidth) {
            constexpr int w = stageWidth;
            constexpr int h = stageHeight;
            const int before = boxWidth / 2;
            const int after = boxWidth - before - 1;
            const float divisor = static_cast<float>(boxWidth);
            std::vector<float> horizontal(mask.size(), 0.0f);
            std::vector<float> blurred(mask.size(), 0.0f);

            for (int y = 0; y < h; ++y) {
                float sum = 0.0f;
                for (int x = -before; x <= after; ++x)
                    if (x >= 0 && x < w) sum += mask[static_cast<size_t>(y) * w + x];
                for (int x = 0; x < w; ++x) {
                    horizontal[static_cast<size_t>(y) * w + x] = sum / divisor;
                    const int removeX = x - before;
                    const int addX = x + after + 1;
                    if (removeX >= 0) sum -= mask[static_cast<size_t>(y) * w + removeX];
                    if (addX < w) sum += mask[static_cast<size_t>(y) * w + addX];
                }
            }

            for (int x = 0; x < w; ++x) {
                float sum = 0.0f;
                for (int y = -before; y <= after; ++y)
                    if (y >= 0 && y < h) sum += horizontal[static_cast<size_t>(y) * w + x];
                for (int y = 0; y < h; ++y) {
                    blurred[static_cast<size_t>(y) * w + x] = sum / divisor;
                    const int removeY = y - before;
                    const int addY = y + after + 1;
                    if (removeY >= 0) sum -= horizontal[static_cast<size_t>(removeY) * w + x];
                    if (addY < h) sum += horizontal[static_cast<size_t>(addY) * w + x];
                }
            }
            mask.swap(blurred);
        };

        const float halfW = 25.0f * localLayout.scale;
        const float halfH = 20.0f * localLayout.scale;
        const float shoulder = halfW * 0.5f;
        const int blurWidth = std::max(2,
            static_cast<int>(std::lround(30.0f * localLayout.scale)));

        for (int faction = 0; faction < kFactionCount; ++faction) {
            // Reblur only factions whose coverage changed; settled borders
            // keep their cached masks throughout another faction's animation.
            std::uint64_t maskKey = static_cast<std::uint64_t>(scaleKey);
            maskKey = maskKey * 1099511628211ULL ^ static_cast<std::uint64_t>(layoutLeftKey);
            maskKey = maskKey * 1099511628211ULL ^ static_cast<std::uint64_t>(layoutTopKey);
            for (int index = 0; index < kTileCount; ++index)
                maskKey = maskKey * 1099511628211ULL ^ static_cast<std::uint64_t>(std::lround(coverage(index, faction) * 255.0f));
            auto& cache = factionMasks[faction];
            auto& mask = cache.mask;
            auto& blurredMask = cache.blurred;
            if (mask.empty() || cache.key != maskKey) {
            mask.assign(pixelCount, 0.0f);
            for (int index = 0; index < kTileCount; ++index) {
                const Tile& tile = game.tiles[index];
                const float alpha = coverage(index, faction);
                if (!tile.land || alpha <= 0.0f) continue;

                const glm::vec2 center = TileCenter(index, localLayout);
                const glm::vec2 corners[6] = {
                    {center.x - halfW, center.y}, {center.x - shoulder, center.y - halfH},
                    {center.x + shoulder, center.y - halfH}, {center.x + halfW, center.y},
                    {center.x + shoulder, center.y + halfH}, {center.x - shoulder, center.y + halfH}
                };
                const int minY = std::max(0, static_cast<int>(std::floor(center.y - halfH)));
                const int maxY = std::min(stageHeight - 1,
                    static_cast<int>(std::ceil(center.y + halfH)));
                for (int y = minY; y <= maxY; ++y) {
                    const float sampleY = y + 0.5f;
                    float intersections[6];
                    int count = 0;
                    for (int side = 0; side < 6; ++side) {
                        const glm::vec2 a = corners[side];
                        const glm::vec2 b = corners[(side + 1) % 6];
                        if ((a.y <= sampleY && b.y > sampleY) ||
                            (b.y <= sampleY && a.y > sampleY)) {
                            intersections[count++] = a.x +
                                (sampleY - a.y) * (b.x - a.x) / (b.y - a.y);
                        }
                    }
                    std::sort(intersections, intersections + count);
                    const size_t row = static_cast<size_t>(y) * stageWidth;
                    for (int pair = 0; pair + 1 < count; pair += 2) {
                        const int minX = std::max(0,
                            static_cast<int>(std::ceil(intersections[pair] - 0.5f)));
                        const int maxX = std::min(stageWidth - 1,
                            static_cast<int>(std::floor(intersections[pair + 1] - 0.5f)));
                        for (int x = minX; x <= maxX; ++x)
                            mask[row + x] = alpha;
                    }
                }
            }

            blurredMask = mask;
            blurMask(blurredMask, blurWidth);
            cache.key = maskKey;
            }
            const glm::vec4 color = FactionTerritoryColor(faction);
            for (int y = 0; y < stageHeight; ++y) {
                for (int x = 0; x < stageWidth; ++x) {
                    const size_t index = static_cast<size_t>(y) * stageWidth + x;
                    if (mask[index] <= 0.0f) continue;

                    // Match the source GlowFilter(color, 0.75, 30, 30, 2,
                    // 1, true, true): inner + knockout leaves just the colored
                    // edge. Strength 2 scales the blurred coverage and is
                    // clamped before the 0.75 alpha applies.
                    const float glowAlpha = std::min(1.0f,
                        (1.0f - blurredMask[index]) * 2.0f) * 0.75f * mask[index];
                    const size_t pixel = index * 4;
                    // Both factions can cover a captured hex during its fade.
                    // Composite them instead of overwriting the departing color.
                    const float oldAlpha = rgba[pixel + 3] / 255.0f;
                    const float combinedAlpha = glowAlpha + oldAlpha * (1.0f - glowAlpha);
                    if (combinedAlpha <= 0.0f) continue;
                    for (int channel = 0; channel < 3; ++channel)
                        rgba[pixel + channel] = static_cast<unsigned char>(std::clamp(
                            (color[channel] * 255.0f * glowAlpha + rgba[pixel + channel] * oldAlpha * (1.0f - glowAlpha)) / combinedAlpha,
                            0.0f, 255.0f));
                    rgba[pixel + 3] = static_cast<unsigned char>(combinedAlpha * 255.0f);
                }
            }
        }

        // OpenGL's first uploaded row is the bottom edge; the game coordinates are y-down.
        const size_t rowBytes = static_cast<size_t>(stageWidth) * 4;
        for (int y = 0; y < stageHeight / 2; ++y) {
            auto top = rgba.begin() + static_cast<size_t>(y) * rowBytes;
            auto bottom = rgba.begin() + static_cast<size_t>(stageHeight - y - 1) * rowBytes;
            std::swap_ranges(top, top + rowBytes, bottom);
        }

        cachedGlow = std::make_unique<Texture>(stageWidth, stageHeight, std::move(rgba));
        cachedOwnerHash = ownerHash;
        cachedScaleKey = scaleKey;
        cachedLayoutLeftKey = layoutLeftKey;
        cachedLayoutTopKey = layoutTopKey;
    }

    if (cachedGlow)
        DrawRectangle(Rect(stageOffset, boardShift * uiScale, stageWidth * uiScale, stageHeight * uiScale),
            *cachedGlow);
}
void UpdateCityNameFade(const BoardLayout& layout, int width, int height, float deltaTime) {
    const glm::vec2 normalized = GetMousePosition();
    const glm::vec2 mouse(normalized.x * width, normalized.y * height);
    // The SWF updates at 12 fps and moves halfway toward the target each
    // frame. Convert that per-frame easing to elapsed time so higher refresh
    // rates keep the same roughly 0.5-1 second fade.
    const float fadeStep = 1.0f - std::pow(0.5f,
        std::max(0.0f, deltaTime) * 12.0f);

    for (int index = 0; index < kTileCount; ++index) {
        const Tile& tile = game.tiles[index];
        float target = 0.0f;
        const glm::vec2 tileCenter = TileCenter(index, layout);
        const float armyHoverDistance = std::abs(mouse.x - tileCenter.x) +
            std::abs(mouse.y - tileCenter.y);
        const float armyInfoTarget = tile.army.present &&
            armyHoverDistance < 100.0f * layout.scale ? 1.0f : 0.0f;
        game.armyInfoAlpha[static_cast<size_t>(index)] +=
            (armyInfoTarget - game.armyInfoAlpha[static_cast<size_t>(index)]) * fadeStep;
        if (!tile.townName.empty() && (tile.estate == Estate::Town || tile.estate == Estate::Port)) {
            // sprite_281's enterFrame script sets town_sign alpha to
            // 125 / (|dx| + |dy|) * 100 - 20. Work in SWF stage units so
            // the fade stays the same at different window scales.
            const float distance = (std::abs(mouse.x - tileCenter.x) +
                std::abs(mouse.y - tileCenter.y)) / std::max(layout.scale, 0.001f);
            target = std::clamp(12500.0f / std::max(distance, 1.0f) - 20.0f,
                0.0f, 100.0f) / 100.0f;
        }
        game.cityNameAlpha[index] = target;
    }
}

// The SWF keeps two 800x600 bitmaps: background_1 (dry land and dry towns)
// and background_2 (the visible green land). Each battle copies background_1
// into background_2 through a rotated e_6.png alpha mask, so repeated battles
// converge on the dry art. These framebuffers hold the same data in board
// units: `dry` is background_1, and `damage` holds premultiplied pixels that
// turn the live green terrain into background_2 when drawn over it.
constexpr float kBattleScarLayerWidth = 25.0f + (kMapWidth - 1) * 37.5f + 25.0f;
constexpr float kBattleScarLayerHeight = 20.0f + (kMapHeight - 1) * 40.0f + 40.0f;
// Pixels per board unit, so the layer stays sharp at the largest board scale.
constexpr float kBattleScarLayerResolution = 2.0f;

struct BattleScarLayer {
    std::unique_ptr<FrameBuffer> dry;
    std::unique_ptr<FrameBuffer> damage;
    std::unique_ptr<FrameBuffer> stamp;
    unsigned int generation = 0;
    size_t appliedScars = 0;
};

std::unique_ptr<FrameBuffer> CreateBattleScarBuffer(float width, float height) {
    return std::make_unique<FrameBuffer>(
        static_cast<int>(std::ceil(width * kBattleScarLayerResolution)),
        static_cast<int>(std::ceil(height * kBattleScarLayerResolution)),
        GL_FilterModes::LINEAR, GL_WrapModes::CLAMP_TO_EDGE, GL_InternalFormats::RGBA8,
        GL_ExternalFormats::RGBA, GL_DataFormats::UNSIGNED_BYTE, 4);
}

// Runs draw with the 2D projection mapping logicalSize board units onto target.
template <typename Draw>
void DrawIntoBattleScarBuffer(FrameBuffer& target, glm::vec2 logicalSize, Draw draw) {
    Camera2D& camera = *extras::activeCamera2D;
    const glm::vec2 previousViewportSize = camera.currentViewportSize;
    camera.currentViewportSize = logicalSize;
    target.startDrawing();
    draw();
    camera.currentViewportSize = previousViewportSize;
    target.stopDrawing();
}

void ClearBattleScarBuffer(FrameBuffer& target) {
    GLfloat previousClearColor[4] = {};
    glGetFloatv(GL_COLOR_CLEAR_VALUE, previousClearColor);
    glBindFramebuffer(GL_FRAMEBUFFER, target.fbo);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glClearColor(previousClearColor[0], previousClearColor[1], previousClearColor[2], previousClearColor[3]);
}

void ApplyBattleScar(BattleScarLayer& layer, const BattleScar& scar) {
    const BoardLayout boardUnits{ 1.0f, 0.0f, 0.0f };
    const glm::vec2 center = TileCenter(scar.tile, boardUnits);
    const float half = kBattleScarSize * 0.5f;

    // Build dry * mask in the stamp buffer: write the flipped and rotated
    // mask's alpha first, then replace RGB with the dry pixels scaled by it.
    ClearBattleScarBuffer(*layer.stamp);
    DrawIntoBattleScarBuffer(*layer.stamp, glm::vec2(kBattleScarSize), [&]() {
        Texture& mask = *artwork.erosionMask;
        glm::vec4 source(scar.flipHorizontal ? static_cast<float>(mask.width) : 0.0f,
            scar.flipVertical ? static_cast<float>(mask.height) : 0.0f,
            scar.flipHorizontal ? -static_cast<float>(mask.width) : static_cast<float>(mask.width),
            scar.flipVertical ? -static_cast<float>(mask.height) : static_cast<float>(mask.height));
        glBlendFuncSeparate(GL_ONE, GL_ZERO, GL_ONE, GL_ZERO);
        DrawRectangle(Rect(0.0f, 0.0f, kBattleScarSize, kBattleScarSize),
            glm::radians(static_cast<float>(scar.rotation)), mask, &source);
        glBlendFuncSeparate(GL_DST_ALPHA, GL_ZERO, GL_ZERO, GL_ONE);
        DrawRectangle(Rect(half - center.x, half - center.y,
            kBattleScarLayerWidth, kBattleScarLayerHeight), *layer.dry->texture);
    });

    // copyPixels(..., mergeAlpha=true) lays that stamp over background_2,
    // then the grave stamps follow on the next erosion frame.
    DrawIntoBattleScarBuffer(*layer.damage,
        glm::vec2(kBattleScarLayerWidth, kBattleScarLayerHeight), [&]() {
        glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        DrawRectangle(Rect(center.x - half, center.y - half, kBattleScarSize, kBattleScarSize),
            *layer.stamp->texture);
        glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        for (const BattleScarGrave& grave : scar.graves) {
            Texture* texture = artwork.graves[static_cast<size_t>(grave.variant)].get();
            if (!texture) continue;
            DrawRectangle(Rect(grave.position.x, grave.position.y,
                static_cast<float>(texture->width), static_cast<float>(texture->height)), *texture);
        }
    });
}

void DrawBattleScars(const BoardLayout& layout) {
    static BattleScarLayer layer;
    if (layer.generation != game.battleScarGeneration) {
        layer.generation = game.battleScarGeneration;
        layer.appliedScars = 0;
    }
    // Erosion copies the empty background_1 and n_NaN.png graves never load.
    if (game.battleScars.empty() || !artwork.erosionMask || game.blankMapNumber) return;

    GLint previousBlendSrcRgb = GL_SRC_ALPHA;
    GLint previousBlendDstRgb = GL_ONE_MINUS_SRC_ALPHA;
    GLint previousBlendSrcAlpha = GL_ONE;
    GLint previousBlendDstAlpha = GL_ONE_MINUS_SRC_ALPHA;
    const GLboolean previousBlendEnabled = glIsEnabled(GL_BLEND);
    glGetIntegerv(GL_BLEND_SRC_RGB, &previousBlendSrcRgb);
    glGetIntegerv(GL_BLEND_DST_RGB, &previousBlendDstRgb);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &previousBlendSrcAlpha);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &previousBlendDstAlpha);
    glEnable(GL_BLEND);

    if (!layer.dry) {
        layer.dry = CreateBattleScarBuffer(kBattleScarLayerWidth, kBattleScarLayerHeight);
        layer.damage = CreateBattleScarBuffer(kBattleScarLayerWidth, kBattleScarLayerHeight);
        layer.stamp = CreateBattleScarBuffer(kBattleScarSize, kBattleScarSize);
    }
    if (layer.appliedScars == 0) {
        // A new map or campaign: repaint background_1 and drop old damage.
        ClearBattleScarBuffer(*layer.dry);
        ClearBattleScarBuffer(*layer.damage);
        DrawIntoBattleScarBuffer(*layer.dry,
            glm::vec2(kBattleScarLayerWidth, kBattleScarLayerHeight), [&]() {
            glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
            // Opaque under the patches, so the masked RGB copy never picks
            // up black from uncovered texels along the board edge.
            DrawRectangle(Rect(0.0f, 0.0f, kBattleScarLayerWidth, kBattleScarLayerHeight),
                Color(147, 93, 52));
            const BoardLayout boardUnits{ 1.0f, 0.0f, 0.0f };
            DrawTerrainPatchesClipped(boardUnits, true);
            // 800x600 gives a unit UI scale, so towns snap to whole board units.
            DrawTownTerrainClipped(boardUnits, 800, 600, true);
        });
    }
    for (; layer.appliedScars < game.battleScars.size(); ++layer.appliedScars)
        ApplyBattleScar(layer, game.battleScars[layer.appliedScars]);

    glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    BeginBoardStencil(layout);
    DrawRectangle(Rect(layout.left, layout.top, kBattleScarLayerWidth * layout.scale,
        kBattleScarLayerHeight * layout.scale), *layer.damage->texture);
    EndBoardStencil();

    glBlendFuncSeparate(previousBlendSrcRgb, previousBlendDstRgb,
        previousBlendSrcAlpha, previousBlendDstAlpha);
    if (previousBlendEnabled) glEnable(GL_BLEND); else glDisable(GL_BLEND);
}

void DrawSelectionMap(const BoardLayout& layout, int width, int height, bool selectionScreen,
    bool showNewspaper = true) {
    UpdateCityNameFade(layout, width, height, static_cast<float>(GetFrameTime()));
    DrawTerrainPatchesClipped(layout);
    DrawTownTerrainClipped(layout, width, height);
    DrawBattleScars(layout);
    // Territory glow sits beneath the source-style ocean bitmap. Its colors
    // therefore blend with the transparent coastal pixels instead of covering
    // the shoreline and port stamps.
    DrawFactionTerritoryGlow(layout, width, height);
    DrawOceanComposite(layout, width, height);
    // In gameplay the frame clip also holds the newspaper (paper at depth 6,
    // contents at depth 8), below the corner shadow at depth 28.
    if (!selectionScreen && showNewspaper)
        DrawGameplayNewspaper(width, height);
    // The frame clip (depth 28 holds shape 284) sits above the sea bitmap and
    // below the field clips. Its shape origin is the board origin.
    if (artwork.boardCornerShadow) {
        DrawRectangle(Rect(layout.left - 88.0f * layout.scale, layout.top - 29.0f * layout.scale,
            851.5f * layout.scale, 489.5f * layout.scale), *artwork.boardCornerShadow);
    }

    if (selectionScreen) {
        std::vector<int> capitalTiles;
        capitalTiles.reserve(kFactionCount);
        for (int faction = 0; faction < kFactionCount; ++faction) {
            capitalTiles.push_back(Index(kCapitals[faction][0], kCapitals[faction][1]));
        }
        DrawYellowTileHighlights(layout, capitalTiles);
    }

    // The source places its frame/grid over the sea bitmap, so draw tile lines
    // over the sea bitmaps as well.
    for (int index = 0; index < kTileCount; ++index) {
        const glm::vec2 center = TileCenter(index, layout);
        if (game.hoveredTile == index && artwork.hoveredHex) {
            DrawTexturedHex(center, layout.scale, *artwork.hoveredHex, Color(255, 255, 255));
        }
    }
    std::vector<int> highlightedTiles;
    highlightedTiles.reserve(game.moveTargets.size() + 1);
    for (int index = 0; index < kTileCount; ++index) {
        const bool moveTarget = std::find(game.moveTargets.begin(), game.moveTargets.end(), index) != game.moveTargets.end();
        if (IsMovableArmy(game.tiles[index]) || index == game.selectedTile || moveTarget)
            highlightedTiles.push_back(index);
    }
    DrawYellowTileHighlights(layout, highlightedTiles);
    // The SWF marks the selected army only through the yellow field
    // highlights; it draws no outline around the selected hex.
    DrawHexGridBatch(layout);
    const bool movingArmyShown = !selectionScreen && game.movingArmyFrom >= 0;
    const auto drawMovingArmy = [&]() {
        const int from = game.movingArmyFrom;
        const int target = game.movingArmyTarget;
        // SWF movement halves the remaining distance every frame at 12 fps.
        const float progress = 1.0f - std::pow(0.5f, game.armyTravelElapsed * 12.0f);
        const glm::vec2 position = glm::mix(TileCenter(from, layout), TileCenter(target, layout), progress);
        Tile moving = game.tiles[from];
        moving.land = game.tiles[target].land;
        DrawArmyMarker(from, moving, position, layout.scale);
    };
    // moveArmy runs putBelow(army, field.army) for battles and merges, so an
    // army sliding onto an occupied field is drawn beneath that field's army.
    const bool movingArmyBelowTarget = movingArmyShown &&
        game.tiles[static_cast<size_t>(game.movingArmyTarget)].army.present;
    const auto drawGhosts = [&](int index, const Tile& tile, glm::vec2 center, bool onTop) {
        if (selectionScreen) return;
        for (const BattleExplosion& explosion : game.explosions) {
            // Sprite 167 frame 4 hides the exploding army's soldier.
            if (explosion.tile != index || explosion.ghostOnTop != onTop || !explosion.ghost.present ||
                explosion.elapsed * 12.0f >= 3.0f)
                continue;
            Tile ghost = tile;
            ghost.army = explosion.ghost;
            DrawArmyMarker(-1, ghost, center, layout.scale);
        }
    };
    const auto drawFieldArmies = [&](int index, const Tile& tile, glm::vec2 center) {
        drawGhosts(index, tile, center, false);
        if (movingArmyBelowTarget && index == game.movingArmyTarget) drawMovingArmy();
        if (index != game.movingArmyFrom) {
            if (index == game.repelledDefenderTile && tile.army.present) {
                Tile shown = tile;
                shown.army = game.repelledDefender;
                DrawArmyMarker(index, shown, center, layout.scale);
            } else {
                DrawArmyMarker(index, tile, center, layout.scale);
            }
        }
        drawGhosts(index, tile, center, true);
    };
    for (int index = 0; index < kTileCount; ++index) {
        const Tile& tile = game.tiles[index];
        const glm::vec2 center = TileCenter(index, layout);
        if (!tile.land) {
            drawFieldArmies(index, tile, center);
            continue;
        }

        if (tile.capitalFor >= 0) {
            DrawTownMarker(center, layout.scale, tile.capitalFor, tile.army.present);
        } else if (tile.estate == Estate::Port && artwork.portMedallion) {
            const float size = (tile.army.present ? 20.0f : 35.0f) * layout.scale;
            const float x = center.x + (tile.army.present ? 18.0f : 0.0f) * layout.scale;
            const float y = center.y + (tile.army.present ? -4.0f : 0.0f) * layout.scale;
            DrawRectangle(Rect(x - size * 0.5f, y - size * 0.5f, size, size), *artwork.portMedallion);
        } else if (tile.estate == Estate::Town && tile.capitalFor < 0) {
            DrawTownMarker(center, layout.scale, tile.capitalFor, tile.army.present);
        }

        const float nameAlpha = game.cityNameAlpha[index];
        if (nameAlpha > 0.015f && !tile.townName.empty() &&
            (tile.estate == Estate::Town || tile.estate == Estate::Port)) {
            // SWF fields 276/277 are the black and white layers of the same
            // label. Font 35's first baseline is 18880/20480 em; composing
            // their field and town_sign transforms yields these anchors in
            // sprite_281 coordinates. Their yMin=-40 bounds cancel the
            // EditText 40-twip inset, and the layer offset comes from the SWF.
            const glm::vec2 blackTownBaseline = center + glm::vec2(1.725f, -14.89375f) * layout.scale;
            const glm::vec2 whiteTownBaseline = center + glm::vec2(0.375f, -15.94375f) * layout.scale;
            // SWF EditText fields 276/277 use embedded Arial Narrow Bold at
            // 14 px, with black and white layers for contrast. The embedded
            // glyphs already carry the weight, so don't synthesize more bold.
            Font* previousCityFont = extras::defaultTextRenderer
                ? extras::defaultTextRenderer->font : nullptr;
            const int previousCityWeight = extras::defaultTextRenderer
                ? extras::defaultTextRenderer->fontWeight : 400;
            if (artwork.arialNarrowFont && extras::defaultTextRenderer)
                extras::defaultTextRenderer->setFont(*artwork.arialNarrowFont);
            if (extras::defaultTextRenderer)
                extras::defaultTextRenderer->setFontWeight(400);
            const float fontSize = 14.0f * layout.scale;
            DrawText(tile.townName, blackTownBaseline,
                fontSize, Color(0, 0, 0, static_cast<int>(255.0f * nameAlpha)),
                TextAlignmentX::CENTER, TextAlignmentY::BASELINE);
            DrawText(tile.townName, whiteTownBaseline, fontSize, Color(255, 255, 255,
                static_cast<int>(255.0f * nameAlpha)),
                TextAlignmentX::CENTER, TextAlignmentY::BASELINE);
            if (extras::defaultTextRenderer)
                extras::defaultTextRenderer->setFontWeight(previousCityWeight);
            if (previousCityFont && extras::defaultTextRenderer)
                extras::defaultTextRenderer->setFont(*previousCityFont);
        }
        drawFieldArmies(index, tile, center);
    }

    if (movingArmyShown && !movingArmyBelowTarget) drawMovingArmy();

    if (!selectionScreen) {
        // Draw above the armies, at the source sprite's centered 170px bounds.
        for (const auto& explosion : game.explosions) {
            const int frame = std::clamp(static_cast<int>(explosion.elapsed * 12.0f), 0, kExplosionFrames - 1);
            const auto& texture = artwork.explosionFrames[frame];
            if (!texture) continue;
            const glm::vec2 center = TileCenter(explosion.tile, layout);
            const float size = 170.0f * layout.scale;
            DrawRectangle(Rect(center.x - size * 0.5f, center.y - size * 0.5f, size, size), *texture);
        }
    }

    for (int faction = 0; faction < kFactionCount; ++faction) {
        const int capital = Index(kCapitals[faction][0], kCapitals[faction][1]);
        const glm::vec2 center = TileCenter(capital, layout);
        // Sprite 232 frame 11 hides the capital hints again.
        if (selectionScreen && game.screen == Screen::Menu && game.capitalTooltipTimerStarted &&
            game.capitalTooltipSecondsRemaining > 0.0f && artwork.capitalTooltip) {
            const float tooltipWidth = artwork.capitalTooltip->width * layout.scale;
            const float tooltipHeight = artwork.capitalTooltip->height * layout.scale;
            // Its last five frames fade the alpha by 0.18 per frame (0.82 down
            // to 0.10); this follows the same line continuously.
            const float framesLeft = game.capitalTooltipSecondsRemaining * 12.0f;
            const float opacity = framesLeft < 5.0f ? 0.1f + 0.18f * framesLeft : 1.0f;
            DrawRectangle(Rect(center.x - tooltipWidth * 0.5f, center.y - 63.0f * layout.scale,
                tooltipWidth, tooltipHeight), glm::vec4(1.0f, 1.0f, 1.0f, opacity),
                *artwork.capitalTooltip);
        }
    }
    const bool armyMoveHintActive = game.armyMoveHintElapsed >= 0.0f;
    if (!selectionScreen && (game.armySelectionHintVisible || armyMoveHintActive) &&
        artwork.armySelectionTooltip) {
        int firstArmy = -1;
        int firstVisibleArmy = -1;
        const float hintScale = std::clamp(std::min(width / 800.0f, height / 600.0f), 0.72f, 1.2f);
        // Sprite 61 is exported as vector shape and embedded-glyph paths. Its
        // 4x SVG raster is resolved to native-size coverage at load time, while
        // layout still follows the authored SWF bounds.
        constexpr float hintNativeWidth = 126.0f;
        constexpr float hintNativeHeight = 110.8f;
        constexpr float hintTopBubbleHeight = 42.0f;
        auto hintBoundsFor = [&](int tileIndex) {
            const glm::vec2 center = TileCenter(tileIndex, layout);
            const float unitTop = center.y - 15.5f * layout.scale;
            return Rect(center.x - hintNativeWidth * hintScale * 0.5f,
                unitTop - (hintTopBubbleHeight - 2.0f) * hintScale,
                hintNativeWidth * hintScale, hintNativeHeight * hintScale);
        };
        if (game.armySelectionHintVisible) {
            // ActionScript listArmies scans x first, then y, and the human-turn
            // hint is attached to the first army in that list. If a narrow window
            // crops that army off-screen, keep the opening hint visible on the
            // first army the player can actually see.
            for (int x = 0; x < kMapWidth && firstVisibleArmy < 0; ++x) {
                for (int y = 0; y < kMapHeight; ++y) {
                    const int index = Index(x, y);
                    const Army& army = game.tiles[static_cast<size_t>(index)].army;
                    if (army.present && army.owner == game.human) {
                        if (firstArmy < 0) firstArmy = index;
                        const Rect candidate = hintBoundsFor(index);
                        if (candidate.position.x + candidate.size.x > 0.0f &&
                            candidate.position.x < static_cast<float>(width) &&
                            candidate.position.y + candidate.size.y > 0.0f &&
                            candidate.position.y < static_cast<float>(height)) {
                            firstVisibleArmy = index;
                            break;
                        }
                    }
                }
            }
            game.armyHintTile = firstVisibleArmy >= 0 ? firstVisibleArmy : firstArmy;
        }
        if (game.armyHintTile >= 0) {
            const Rect tooltipBounds = hintBoundsFor(game.armyHintTile);
            // Once the army is clicked, frames 2-6 fade the lower Manpower
            // bubble (0.8 down to 0) while the upper bubble switches to text 60;
            // frames 35-39 fade the upper bubble (0.82 down to 0.10).
            float topOpacity = 1.0f;
            float bottomOpacity = 1.0f;
            Texture* topTexture = artwork.armySelectionTooltip.get();
            if (armyMoveHintActive) {
                const float frame = 1.0f + game.armyMoveHintElapsed * 12.0f;
                bottomOpacity = std::clamp(1.0f - 0.2f * (frame - 1.0f), 0.0f, 1.0f);
                topOpacity = frame < 34.0f ? 1.0f : std::max(0.0f, 1.0f - 0.18f * (frame - 34.0f));
                if (artwork.armyMoveTooltip) topTexture = artwork.armyMoveTooltip.get();
            }
            // The bubbles don't overlap: split the frame between them at y=56.
            constexpr float splitY = 56.0f;
            auto drawPart = [](Texture& texture, const Rect& bounds, float fromFraction,
                float toFraction, float opacity) {
                if (opacity <= 0.0f) return;
                const float textureWidth = static_cast<float>(texture.width);
                const float textureHeight = static_cast<float>(texture.height);
                glm::vec4 source(0.0f, (1.0f - toFraction) * textureHeight, textureWidth,
                    (toFraction - fromFraction) * textureHeight);
                DrawRectangle(Rect(bounds.position.x, bounds.position.y + fromFraction * bounds.size.y,
                    bounds.size.x, (toFraction - fromFraction) * bounds.size.y),
                    glm::vec4(1.0f, 1.0f, 1.0f, opacity), texture, &source);
            };
            if (artwork.armySelectionTooltipShadow) {
                // The shadow mask carries a 7 px pad around the same frame.
                Texture& shadow = *artwork.armySelectionTooltipShadow;
                constexpr float shadowPad = 7.0f;
                const float unit = tooltipBounds.size.x / hintNativeWidth;
                const Rect shadowBounds(tooltipBounds.position.x - shadowPad * unit,
                    tooltipBounds.position.y - shadowPad * unit,
                    (hintNativeWidth + shadowPad * 2.0f) * unit,
                    (hintNativeHeight + shadowPad * 2.0f) * unit);
                const float shadowSplit = (splitY + shadowPad) / (hintNativeHeight + shadowPad * 2.0f);
                drawPart(shadow, shadowBounds, 0.0f, shadowSplit, topOpacity);
                drawPart(shadow, shadowBounds, shadowSplit, 1.0f, bottomOpacity);
            }
            const float split = splitY / hintNativeHeight;
            drawPart(*topTexture, tooltipBounds, 0.0f, split, topOpacity);
            drawPart(*artwork.armySelectionTooltip, tooltipBounds, split, 1.0f, bottomOpacity);
        }
    }
    DrawHexBoardEdge(layout);
}
// Sprite 102 places its layers in sprite-local pixels; sprite 91 (`main`)
// sits at (-19.25, -40.2), so its 39.35 x 47.4 colour shape (86, 88-90)
// spans the shield bounds the callers pass.
constexpr glm::vec2 kBadgeShieldOrigin(-19.25f, -40.2f);
constexpr glm::vec2 kBadgeShieldSize(39.35f, 47.4f);

struct BadgeSpace {
    glm::vec2 origin;  // screen position of the badge clip's origin
    glm::vec2 unit;    // screen pixels per sprite-local pixel
    glm::vec4 tint;

    static BadgeSpace FromShield(Rect shield, float opacity) {
        const glm::vec2 unit = shield.size / kBadgeShieldSize;
        const int alpha = static_cast<int>(std::lround(std::clamp(opacity, 0.0f, 1.0f) * 255.0f));
        return { shield.position - kBadgeShieldOrigin * unit, unit, Color(255, 255, 255, alpha) };
    }

    // Draws a shape whose bounds are (x, y, w, h) in its own pixels, placed
    // with the SWF matrix (a, b, c, d, tx, ty) in sprite-local pixels.
    void draw(Texture& texture, glm::vec4 shapeBounds, float a = 1.0f, float b = 0.0f,
              float c = 0.0f, float d = 1.0f, float tx = 0.0f, float ty = 0.0f) const {
        const glm::vec2 shapeCenter(shapeBounds.x + shapeBounds.z * 0.5f, shapeBounds.y + shapeBounds.w * 0.5f);
        const glm::vec2 local(a * shapeCenter.x + c * shapeCenter.y + tx, b * shapeCenter.x + d * shapeCenter.y + ty);
        const glm::vec2 center = origin + local * unit;
        const bool mirrored = a * d - b * c < 0.0f;
        const float scaleX = std::sqrt(a * a + b * b) * (mirrored ? -1.0f : 1.0f);
        const float scaleY = std::sqrt(c * c + d * d);
        const float angle = mirrored ? std::atan2(-b, -a) : std::atan2(b, a);
        // A negative width mirrors the quad about its centre before rotating.
        const float width = shapeBounds.z * scaleX * unit.x;
        const float height = shapeBounds.w * scaleY * unit.y;
        DrawRectangle(Rect(center.x - std::abs(width) * 0.5f, center.y - height * 0.5f, width, height),
            angle, tint, texture);
    }
};

// Sprite 91: colour shape 86/88/89/90 by party, with outline 87 above it.
void DrawShieldPiece(const BadgeSpace& space, int faction, float a, float b, float c, float d, float tx, float ty) {
    if (faction < 0 || static_cast<size_t>(faction) >= artwork.factionShields.size()) return;
    if (const auto& color = artwork.factionShields[static_cast<size_t>(faction)])
        space.draw(*color, { 0.0f, 0.0f, 39.35f, 47.4f }, a, b, c, d, tx, ty);
    if (artwork.factionShieldOutline)
        space.draw(*artwork.factionShieldOutline, { -0.5f, -0.5f, 40.35f, 48.4f }, a, b, c, d, tx, ty);
}

// Shape 92, the gloss over the shield's top half, at sprite depth 17.
void DrawShieldHighlight(const BadgeSpace& space) {
    if (artwork.factionShieldHighlight)
        space.draw(*artwork.factionShieldHighlight, { -19.0f, -40.0f, 39.0f, 21.0f });
}

void DrawFactionShield(int faction, Rect bounds, bool highlighted, float scale, float opacity = 1.0f,
                       bool withHighlight = true) {
    const float x = bounds.position.x, y = bounds.position.y;
    const float w = bounds.size.x, h = bounds.size.y;
    if (faction >= 0 && static_cast<size_t>(faction) < artwork.factionShields.size() &&
        artwork.factionShields[static_cast<size_t>(faction)]) {
        const BadgeSpace space = BadgeSpace::FromShield(bounds, opacity);
        // Shape 85, the dark backing, sits at the badge origin.
        if (artwork.factionShieldShadowBezel)
            space.draw(*artwork.factionShieldShadowBezel, { -27.85f, -42.0f, 56.0f, 67.7f });
        DrawShieldPiece(space, faction, 1.0f, 0.0f, 0.0f, 1.0f, kBadgeShieldOrigin.x, kBadgeShieldOrigin.y);
        if (withHighlight)
            DrawShieldHighlight(space);
        return;
    }
    glm::vec4 color = highlighted ? FactionLight(faction) : FactionColor(faction);
    color = glm::mix(color, Color(0, 0, 0), 0.2f);
    DrawRectangle(Rect(x + 1.0f * scale, y, w - 2.0f * scale, h * 0.53f), color);
    DrawTriangle(glm::vec3(x + 1.0f * scale, y + h * 0.48f, 0),
        glm::vec3(x + w - 1.0f * scale, y + h * 0.48f, 0),
        glm::vec3(x + w * 0.5f, y + h, 0), color);
    std::vector<glm::vec2> outline = {
        {x + 2.0f * scale, y}, {x + w - 2.0f * scale, y},
        {x + w - 2.0f * scale, y + h * 0.54f}, {x + w * 0.83f, y + h * 0.81f},
        {x + w * 0.5f, y + h}, {x + w * 0.17f, y + h * 0.81f},
        {x + 2.0f * scale, y + h * 0.54f}, {x + 2.0f * scale, y}
    };
    BatchDrawLines(outline, highlighted ? Color(255, 249, 222) : Color(145, 149, 144), 1.0f * scale);
}

struct FactionBadgeState {
    int status = 0;
    std::vector<int> capturedCapitalOwners;
};

FactionBadgeState GetFactionBadgeState(int faction) {
    FactionBadgeState result;
    if (faction < 0 || faction >= kFactionCount) return result;
    if (!IsAlive(faction)) return result;

    result.status = 1;
    for (int formerOwner = 0; formerOwner < kFactionCount; ++formerOwner) {
        if (formerOwner == faction || ArmyCount(formerOwner) > 0) continue;
        const int capital = Index(kCapitals[formerOwner][0], kCapitals[formerOwner][1]);
        if (game.tiles[capital].owner == faction)
            result.capturedCapitalOwners.push_back(formerOwner);
    }
    result.status = std::min(kFactionCount, 1 + static_cast<int>(result.capturedCapitalOwners.size()));
    return result;
}

std::string FactionBadgeTitle(int status) {
    if (status <= 0) return "Province";
    if (status == 1) return "Kingdom";
    return "Empire";
}

FactionBadgeState DrawFactionBadgeArt(int faction, Rect shieldBounds, bool highlighted, float scale, float opacity) {
    if (faction < 0 || faction >= kFactionCount) return {};
    const FactionBadgeState badgeState = GetFactionBadgeState(faction);
    const BadgeSpace space = BadgeSpace::FromShield(shieldBounds, opacity);

    DrawFactionShield(faction, shieldBounds, highlighted, scale, opacity, false);

    // The enterFrame script shows `central` at status 2, `left` and `right`
    // at status 3 and all three at status 4, each set to a captured capital.
    const auto& captured = badgeState.capturedCapitalOwners;
    const auto owner = [&](size_t index) { return index < captured.size() ? captured[index] : -1; };
    const int central = badgeState.status == 2 || badgeState.status == 4 ? owner(0) : -1;
    const int left = badgeState.status == 3 ? owner(0) : badgeState.status == 4 ? owner(1) : -1;
    const int right = badgeState.status == 3 ? owner(1) : badgeState.status == 4 ? owner(2) : -1;
    DrawShieldPiece(space, central, 0.4594f, 0.0f, 0.0f, 0.4594f, -9.05f, -28.95f);
    DrawShieldPiece(space, right, 0.3978f, 0.2297f, -0.2297f, 0.3978f, 23.5f, -13.25f);
    DrawShieldPiece(space, left, -0.3978f, 0.2297f, 0.2297f, 0.3978f, -23.2f, -13.25f);

    DrawShieldHighlight(space);

    // `lions` (sprite 94 at -54, -54) holds bitmap shape 93 at (55, 28).
    if (badgeState.status >= 2 && artwork.heraldicLions)
        space.draw(*artwork.heraldicLions, { -55.0f, -28.0f, 110.0f, 56.0f }, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f, -26.0f);
    // `crown` (sprite 96 at -20, -66) holds bitmap shape 95 at (20, 11).
    if (badgeState.status >= 1 && artwork.royalCrown)
        space.draw(*artwork.royalCrown, { -20.0f, -11.0f, 40.0f, 23.0f }, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, -55.0f);

    // These overlays are independent of kingdom/empire status in the source.
    if (faction == game.human && artwork.humanCrossedSwords)
        space.draw(*artwork.humanCrossedSwords, { -13.7f, -13.05f, 27.4f, 26.15f },
            1.0f, 0.0f, 0.0f, 0.998f, 0.5f, -21.0f);
    if (faction == game.pactPartner && artwork.pactPeaceEmblem)
        space.draw(*artwork.pactPeaceEmblem, { -13.8f, -18.0f, 31.4f, 30.8f },
            1.0f, 0.0f, 0.0f, 1.0f, 0.95f, -18.7f);
    return badgeState;
}

struct GameplayControls {
    Rect endTurn;
    Rect speech;
    Rect pact;
    Rect capitulate;
    Rect help;
};

struct PactDialogControls {
    float scale;
    Rect stage;
    Rect cancel;
    std::array<Rect, kFactionCount> parties;
    // The red sprite 43 on the "fail"/"success" frame, as its 98x32 plate.
    Rect result;
};

struct SpeechDialogControls {
    float scale = 1.0f;
    Rect panel;
    Rect confirm;
    Rect cancel;
};

SpeechDialogControls GetSpeechDialogControls(int width, int height) {
    const float scale = std::clamp(std::min(width / 800.0f, height / 600.0f), 0.5f, 1.2f);
    const float offsetX = (width - 800.0f * scale) * 0.5f;
    const float offsetY = (height - 600.0f * scale) * 0.5f;
    auto designRect = [=](float x, float y, float w, float h) {
        return Rect(offsetX + x * scale, offsetY + y * scale, w * scale, h * scale);
    };
    return { scale,
        designRect(141.0f, 157.0f, 516.0f, 285.0f),
        designRect(172.0f, 405.0f, 91.0f, 27.0f),
        designRect(270.0f, 405.0f, 91.0f, 27.0f) };
}

GameplayControls GetGameplayControls(int width, int height) {
    const float scale = std::clamp(std::min(width / 800.0f, height / 600.0f), 0.72f, 1.2f);
    const float stageOffset = (width - 800.0f * scale) * 0.5f;
    // Buttons end_turn/give_speech/sign_pact/capitulate sit at x=672.5.
    const float x = stageOffset + 672.5f * scale;
    return {
        Rect(x, 6.0f * scale, 98.0f * scale, 32.0f * scale),
        Rect(x, 32.0f * scale, 98.0f * scale, 32.0f * scale),
        Rect(x, 58.0f * scale, 98.0f * scale, 32.0f * scale),
        Rect(x, 84.0f * scale, 98.0f * scale, 32.0f * scale),
        Rect(stageOffset + 771.0f * scale, 244.0f * scale,
            32.0f * scale, 32.0f * scale)
    };
}

std::array<Rect, 4> GetGameplayActionButtonRects(const GameplayControls& controls) {
    return { controls.endTurn, controls.speech, controls.pact, controls.capitulate };
}

void UpdateGameplayActionButtons(const GameplayControls& controls, glm::vec2 mouse) {
    const std::array<Rect, 4> rects = GetGameplayActionButtonRects(controls);
    const std::array<const char*, 4> labels = {
        "END TURN", "give a speech", "sign a pact", "capitulate"
    };
    const float scale = controls.endTurn.size.y / 32.0f;
    for (size_t index = 0; index < rects.size(); ++index) {
        std::unique_ptr<Button>& button = game.gameplayActionButtons[index];
        if (!button) {
            button = std::make_unique<Button>(rects[index], 13.0f * scale,
                labels[index], Color(55, 55, 55));
        }
        button->rect = rects[index];
        button->Update(mouse);
    }
}

PactDialogControls GetPactDialogControls(int width, int height) {
    const float scale = std::clamp(std::min(width / 800.0f, height / 600.0f), 0.72f, 1.2f);
    const Rect stage((width - 900.0f * scale) * 0.5f,
        (height - 600.0f * scale) * 0.5f, 900.0f * scale, 600.0f * scale);
    const std::array<float, kFactionCount> centers = { 376.0f, 491.0f, 606.0f, 720.0f };
    std::array<Rect, kFactionCount> parties{};
    for (int faction = 0; faction < kFactionCount; ++faction) {
        parties[static_cast<size_t>(faction)] = Rect(
            stage.position.x + (centers[static_cast<size_t>(faction)] - 43.0f) * scale,
            stage.position.y + 322.0f * scale, 86.0f * scale, 86.0f * scale);
    }
    // Sprite 43 sits at (-148, 33.4) on "fail" and (-203, 103.4) on "success"
    // in sprite 435, whose origin is the centre of the 900x600 stage.
    const glm::vec2 resultButton = game.pactDialogPage == PactDialogPage::Signed
        ? glm::vec2(247.0f, 403.4f) : glm::vec2(302.0f, 333.4f);
    return { scale, stage,
        Rect(stage.position.x + 112.0f * scale, stage.position.y + 421.0f * scale,
            92.0f * scale, 25.0f * scale), parties,
        Rect(stage.position.x + resultButton.x * scale, stage.position.y + resultButton.y * scale,
            98.0f * scale, 32.0f * scale) };
}

bool IsPactPartyAvailable(int faction) {
    return faction >= 0 && faction < kFactionCount && faction != game.human && IsAlive(faction);
}

void DrawNewspaper(const BoardLayout& layout, float paperAlpha = 1.0f, bool showContents = true) {
    // Sprite 354 ("contents" in the board's frame clip), drawn from
    // its pieces in board-local SWF coordinates. Matrices are (a, b) with
    // c = -b, d = a; each piece rotates around its own origin.
    Text* renderer = extras::defaultTextRenderer;
    const float scale = layout.scale;
    auto boardPoint = [&](glm::vec2 point) {
        return glm::vec2(layout.left + point.x * scale, layout.top + point.y * scale);
    };
    auto rotate = [](glm::vec2 v, float a, float b) {
        return glm::vec2(a * v.x - b * v.y, b * v.x + a * v.y);
    };
    auto drawShape = [&](Texture* texture, glm::vec2 origin, float a = 1.0f, float b = 0.0f) {
        if (!texture) return;
        const glm::vec2 size = glm::vec2(texture->width, texture->height) * scale;
        const glm::vec2 center = boardPoint(origin) + rotate(size * 0.5f, a, b);
        DrawRectangle(Rect(center - size * 0.5f, size), std::atan2(b, a), *texture);
    };
    // Paper backing (shape 296), then the article's rules, columns and photo.
    if (Texture* backing = artwork.newspaperBacking.get(); backing && paperAlpha > 0.0f) {
        const glm::vec2 size = glm::vec2(backing->width, backing->height) * scale;
        DrawRectangle(Rect(boardPoint(glm::vec2(-42.75f, -111.5f)), size),
            glm::vec4(1.0f, 1.0f, 1.0f, paperAlpha), *backing);
    }
    if (!showContents) return;
    // Mask 283 clips the contents to the paper, whose notched lower edge
    // follows the board's first hex row: keep them off the board's cells.
    glEnable(GL_STENCIL_TEST);
    glStencilMask(0xFF);
    glClearStencil(0);
    glClear(GL_STENCIL_BUFFER_BIT);
    glStencilFunc(GL_ALWAYS, 1, 0xFF);
    glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    DrawHexFillBatch(layout, Color(255, 255, 255), [](int) { return true; });
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glStencilMask(0x00);
    glStencilFunc(GL_NOTEQUAL, 1, 0xFF);
    drawShape(artwork.newspaperRules.get(), glm::vec2(-17.1f, -103.5f));
    if (!renderer) {
        glStencilMask(0xFF);
        glDisable(GL_STENCIL_TEST);
        return;
    }

    Font* previousFont = renderer->font;
    const int previousWeight = renderer->fontWeight;
    const float previousStroke = renderer->strokeWidth;
    const float previousHorizontalScale = renderer->horizontalScale;
    renderer->setFontWeight(400);
    renderer->setStrokeWidth(0.0f);
    renderer->setHorizontalScale(1.0f);
    renderer->setColor(Color(0, 0, 0));
    auto drawText = [&](Font* font, float fontSize, const std::string& text, glm::vec2 anchor,
        float a, float b, TextAlignmentX alignment) {
        if (!font) return;
        renderer->setFont(*font);
        renderer->setCharacterSize(fontSize * scale);
        renderer->setString(text);
        const glm::vec2 point = boardPoint(anchor);
        renderer->DrawPrecise(point.x, point.y, alignment, TextAlignmentY::BASELINE, std::atan2(b, a));
    };
    // Static text runs use their recorded glyph advances (twips).
    auto drawRun = [&](Font* font, float fontSize, const char* text,
        std::initializer_list<int> advancesTwips, glm::vec2 pen, float a, float b) {
        float advance = 0.0f;
        const int* step = advancesTwips.begin();
        for (const char* c = text; *c && step != advancesTwips.end(); ++c, ++step) {
            drawText(font, fontSize, std::string(1, *c), pen + rotate(glm::vec2(advance, 0.0f), a, b),
                a, b, TextAlignmentX::LEFT);
            advance += *step / 20.0f;
        }
    };
    Font* caslon = artwork.selectionFont.get();
    const std::string headline = game.news.empty() ? "Peace is not worth the effort!" : game.news.front();
    const std::string alternateHeadline = "Long live " + kFactions[game.human] + "!";
#include "NewspaperLayout.inc"
    // Static text 306 "moves:", 12 px, followed by field 308 (bold digits).
    drawRun(caslon, 12.0f, "moves:", { 193, 121, 103, 102, 85, 61 },
        glm::vec2(-14.746f, -59.504f), 0.998642f, -0.048843f);
    drawText(artwork.newspaperBoldDigitsFont ? artwork.newspaperBoldDigitsFont.get() : caslon,
        12.0f, std::to_string(game.movesLeft), glm::vec2(19.556f, -60.981f),
        0.998642f, -0.048843f, TextAlignmentX::LEFT);
    // Field 305 (variable "turn") right-aligns its line end at this point.
    drawText(caslon, 12.0f, "turn:" + std::to_string(game.turns), glm::vec2(141.588f, -66.391f),
        0.998642f, -0.048843f, TextAlignmentX::RIGHT);
    // Field 303 (variable "name"), Potsdam 18 px, centered.
    drawText(artwork.newspaperTitleFont ? artwork.newspaperTitleFont.get() : caslon, 18.0f,
        kFactions[game.human] + std::string(" Times"), glm::vec2(63.21f, -79.095f),
        0.998886f, -0.043869f, TextAlignmentX::CENTER);

    renderer->setHorizontalScale(previousHorizontalScale);
    renderer->setStrokeWidth(previousStroke);
    renderer->setFontWeight(previousWeight);
    if (previousFont)
        renderer->setFont(*previousFont);
    glStencilMask(0xFF);
    glDisable(GL_STENCIL_TEST);
}

int ResultTimelineFrame() {
    return static_cast<int>(game.resultElapsed * 12.0f);
}

// Frame 5 ("lost") is the first with the panel; frames 1-4 fade the modal in.
constexpr int kResultPanelFrame = 4;

// The board's frame clip (sprite 359). frame.play() at game start shows the
// contents on its frame 2 and fades the paper (depth 6) and help button
// (depth 35) in by 0.25 per frame; sprite 231 frame 5 plays it on, fading them
// out over four frames before frame 11 removes the contents too.
float BoardFrameFadeAlpha() {
    if (game.screen == Screen::Result) {
        const int frame = ResultTimelineFrame() - kResultPanelFrame;
        return std::clamp(1.0f - 0.25f * static_cast<float>(frame), 0.0f, 1.0f);
    }
    const int frame = static_cast<int>(game.gameplayElapsed * 12.0f);
    return std::clamp(0.25f * static_cast<float>(frame - 1), 0.0f, 1.0f);
}

bool BoardFrameContentsVisible() {
    if (game.screen == Screen::Result)
        return ResultTimelineFrame() - kResultPanelFrame < 5;
    return game.gameplayElapsed * 12.0f >= 1.0f;
}

void DrawGameplayNewspaper(int width, int height) {
    if (!BoardFrameContentsVisible()) return;
    DrawNewspaper(GetBoardLayout(width, height), BoardFrameFadeAlpha(), true);
}

void DrawBoardFrameHelpIcon(int width, int height, bool helpHovered, float fade);

void DrawGameplayHeader(int width, int height, glm::vec2 mouse) {
    const float scale = std::clamp(std::min(width / 800.0f, height / 600.0f), 0.72f, 1.2f);
    const GameplayControls controls = GetGameplayControls(width, height);
    const float stageOffset = (width - 800.0f * scale) * 0.5f;
    // Gameplay frame 19 places the four badge clips (sprite 102) at these
    // origins on y=79; the clip is the same one used by the selection footer.
    constexpr std::array<float, kFactionCount> badgeOriginX = { 247.0f, 360.0f, 474.0f, 587.0f };
    for (int faction = 0; faction < kFactionCount; ++faction) {
        const size_t party = static_cast<size_t>(faction);
        const bool isTurn = faction == game.currentPlayer;
        const float targetScale = isTurn ? 1.0f : 0.75f;
        const float targetOpacity = isTurn ? 1.0f : IsAlive(faction) ? 0.75f : 0.3f;
        game.gameplayBadgeScales[party] += (targetScale - game.gameplayBadgeScales[party]) * 0.5f;
        game.gameplayBadgeOpacities[party] += (targetOpacity - game.gameplayBadgeOpacities[party]) * 0.5f;
        const float badgeScale = game.gameplayBadgeScales[party];
        const float badgeOpacity = game.gameplayBadgeOpacities[party];
        const glm::vec2 clipOrigin(stageOffset + badgeOriginX[party] * scale, 79.0f * scale);
        const float badgeUnit = scale * badgeScale;
        const Rect shield(clipOrigin + kBadgeShieldOrigin * badgeUnit, kBadgeShieldSize * badgeUnit);
        const FactionBadgeState badgeState =
            DrawFactionBadgeArt(faction, shield, isTurn, scale, badgeOpacity);

        // Field 97 (name_field): white Arial Narrow Bold at 12 px, centered
        // 0.6 px left of the clip origin with its baseline 23.312 px below it.
        if (Text* renderer = extras::defaultTextRenderer) {
            Font* previousFont = renderer->font;
            const int previousWeight = renderer->fontWeight;
            const float previousStroke = renderer->strokeWidth;
            if (artwork.arialNarrowFont)
                renderer->setFont(*artwork.arialNarrowFont);
            renderer->setStrokeWidth(0.0f);
            // Same ink calibration as the selection footer's captions.
            const float hoverAmount = std::clamp((badgeScale - 0.75f) / 0.25f, 0.0f, 1.0f);
            renderer->setFontWeight(static_cast<int>(std::lround(400.0f + 70.0f * hoverAmount)));
            renderer->setHorizontalScale(1.0f);
            renderer->setCharacterSize(12.0f * scale * badgeScale);
            renderer->setColor(Color(255, 255, 255,
                static_cast<int>(std::lround(badgeOpacity * 255.0f))));
            renderer->setString(FactionBadgeTitle(badgeState.status) + " of " + kFactions[party]);
            renderer->DrawPrecise(clipOrigin.x - 0.6f * scale * badgeScale,
                clipOrigin.y + 23.312f * scale * badgeScale,
                TextAlignmentX::CENTER, TextAlignmentY::BASELINE);
            renderer->setStrokeWidth(previousStroke);
            renderer->setFontWeight(previousWeight);
            if (previousFont)
                renderer->setFont(*previousFont);
        }
    }

    const std::array<Rect, 4> actionButtonRects = GetGameplayActionButtonRects(controls);
    int hoveredActionButton = -1;
    for (int index = static_cast<int>(actionButtonRects.size()) - 1; index >= 0; --index) {
        const auto& button = game.gameplayActionButtons[static_cast<size_t>(index)];
        if (button && (button->state == Button::States::HOVERING ||
            button->state == Button::States::HELD || button->state == Button::States::PRESSED)) {
            hoveredActionButton = index;
            break;
        }
    }
    auto state = [&](int index) {
        // Sprite 81 frame 4 only deactivates these buttons on computer turns.
        if (game.currentPlayer != game.human) return ButtonVisualState::Inactive;
        if (index != hoveredActionButton) return ButtonVisualState::Normal;
        const auto& button = game.gameplayActionButtons[static_cast<size_t>(index)];
        if (button && button->state == Button::States::HELD)
            return ButtonVisualState::Pressed;
        return ButtonVisualState::Hovered;
    };
    DrawPillButton(controls.endTurn, "END TURN", true, state(0),
        scale, Color(255, 255, 255, 204), 255, 32.0f);
    DrawPillButton(controls.speech, "give a speech", false,
        game.speechUsed ? ButtonVisualState::Inactive : state(1),
        scale, game.speechUsed ? Color(255, 255, 255, 128) : Color(0, 0, 0, 192),
        255, 32.0f);
    DrawPillButton(controls.pact, "sign a pact", false,
        game.pactButtonInactive ? ButtonVisualState::Inactive : state(2),
        scale, Color(0, 0, 0, 192), 255, 32.0f);
    DrawPillButton(controls.capitulate, "capitulate", false, state(3),
        scale, Color(0, 0, 0, 192), 255, 32.0f);

    DrawBoardFrameHelpIcon(width, height, Contains(controls.help, mouse), BoardFrameFadeAlpha());
}

// Button 358 in the board's frame clip: static text "i" in Webdings at 16 px,
// scaled 1.19, 50% white (opaque when hovered). Its baseline lands at
// (775.88, 262.75) on the stage. The frame clip fades it with the newspaper.
void DrawBoardFrameHelpIcon(int width, int height, bool helpHovered, float fade) {
    if (fade <= 0.0f) return;
    const float scale = std::clamp(std::min(width / 800.0f, height / 600.0f), 0.72f, 1.2f);
    const float stageOffset = (width - 800.0f * scale) * 0.5f;
    Text* renderer = extras::defaultTextRenderer;
    if (artwork.helpIconFont && renderer) {
        Font* previousFont = renderer->font;
        const int previousWeight = renderer->fontWeight;
        const float previousStroke = renderer->strokeWidth;
        renderer->setFont(*artwork.helpIconFont);
        renderer->setFontWeight(400);
        renderer->setStrokeWidth(0.0f);
        renderer->setCharacterSize(16.0f * 1.19f * scale);
        renderer->setColor(Color(255, 255, 255,
            static_cast<int>(std::lround((helpHovered ? 255.0f : 128.0f) * fade))));
        renderer->setString("i");
        renderer->DrawPrecise(stageOffset + 775.88f * scale, 262.75f * scale,
            TextAlignmentX::LEFT, TextAlignmentY::BASELINE);
        renderer->setStrokeWidth(previousStroke);
        renderer->setFontWeight(previousWeight);
        if (previousFont)
            renderer->setFont(*previousFont);
    }
}

void DrawOpeningGameplayTooltips(int width, int height) {
    const float scale = std::clamp(std::min(width / 800.0f, height / 600.0f), 0.72f, 1.2f);
    const GameplayControls controls = GetGameplayControls(width, height);
    const float stageOffset = (width - 800.0f * scale) * 0.5f;
    // Clips h0/h1/h2 (sprites 211, 213, 215). These rects are their exported
    // frames, placed to match the SWF render. Their last five frames fade the
    // alpha by 0.18 per frame (0.82 down to 0.10); this follows the same line.
    const float framesLeft = game.openingTooltipSecondsRemaining * 12.0f;
    const float opacity = framesLeft < 5.0f ? 0.1f + 0.18f * framesLeft : 1.0f;
    const glm::vec4 tint(1.0f, 1.0f, 1.0f, opacity);
    if (game.movesLeftHintVisible && artwork.movesLeftTooltip)
        DrawRectangle(Rect(stageOffset + 44.0f * scale, 34.0f * scale,
            133.0f * scale, 33.0f * scale), tint, *artwork.movesLeftTooltip);
    if (game.specialButtonsHintVisible && artwork.specialButtonsTooltip)
        DrawRectangle(Rect(stageOffset + 540.0f * scale, 41.0f * scale,
            133.0f * scale, 33.0f * scale), tint, *artwork.specialButtonsTooltip);
    if (game.helpHintVisible && artwork.helpTooltip) {
        const Rect tooltipBounds(stageOffset + 643.0f * scale,
            242.0f * scale, 133.0f * scale, 33.0f * scale);
        if (artwork.helpTooltipShadow)
            DrawTooltipShadow(*artwork.helpTooltipShadow, tooltipBounds, 133.0f, 33.0f, opacity);
        DrawRectangle(tooltipBounds, tint, *artwork.helpTooltip);
    }
}

void DrawSpeechDialog(int width, int height, glm::vec2 mouse) {
    const SpeechDialogControls controls = GetSpeechDialogControls(width, height);
    const float scale = controls.scale;
    DrawRectangle(Rect(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height)),
        Color(0, 0, 0, 145));

    if (artwork.speechDialogFrame)
        DrawRectangle(controls.panel, *artwork.speechDialogFrame);
    else
        DrawRectangle(controls.panel, Color(239, 238, 216));

    const float offsetX = (width - 800.0f * scale) * 0.5f;
    const float offsetY = (height - 600.0f * scale) * 0.5f;
    auto designPoint = [=](float x, float y) {
        return glm::vec2(offsetX + x * scale, offsetY + y * scale);
    };
    const Rect photoRect(offsetX + 184.0f * scale, offsetY + 199.0f * scale,
        189.0f * scale, 189.0f * scale);
    if (artwork.speechDialogPhoto)
        DrawRectangle(photoRect, *artwork.speechDialogPhoto);
    else
        DrawRectangle(photoRect, Color(66, 64, 55));
    DrawRectangleWires(photoRect, Color(72, 69, 59));

    Font* previousFont = extras::defaultTextRenderer
        ? extras::defaultTextRenderer->font : nullptr;
    if (artwork.selectionFont && extras::defaultTextRenderer)
        extras::defaultTextRenderer->setFont(*artwork.selectionFont);
    auto drawBoldText = [&](const std::string& text, glm::vec2 position, float fontSize,
        glm::vec4 color, TextAlignmentX alignmentX, TextAlignmentY alignmentY) {
        const float emboldenOffset = 0.45f * scale;
        DrawText(text, glm::vec2(position.x - emboldenOffset, position.y), fontSize,
            color, alignmentX, alignmentY);
        DrawText(text, position, fontSize, color, alignmentX, alignmentY);
        DrawText(text, glm::vec2(position.x + emboldenOffset, position.y), fontSize,
            color, alignmentX, alignmentY);
    };
    drawBoldText("Give a speech.", designPoint(400.0f, 231.0f),
        29.0f * scale, Color(13, 12, 10), TextAlignmentX::LEFT, TextAlignmentY::CENTER);

    const std::array<const char*, 6> description = {
        "Give a ravishing, enchanting",
        "speech that will raise morale",
        "of your army. They will buy",
        "your rubbish only once per",
        "game, so use this opportunity",
        "carefully."
    };
    for (size_t line = 0; line < description.size(); ++line) {
        drawBoldText(description[line], designPoint(400.0f, 259.0f + 22.0f * static_cast<float>(line)),
            17.0f * scale, Color(17, 15, 13), TextAlignmentX::LEFT, TextAlignmentY::CENTER);
    }

    auto drawDialogButton = [&](Rect rect, bool red, const char* label) {
        const bool hovered = Contains(rect, mouse);
        const bool pressed = hovered && IsMouseButtonDown(GLFW_MOUSE_BUTTON_LEFT);
        Texture* button = red ? artwork.redButton.get() : artwork.silverButton.get();
        if (button) {
            DrawRectangle(rect, *button);
            if (pressed) {
                DrawRectangle(rect, Color(0, 0, 0, 42), *button);
            } else if (hovered) {
                Texture* hoverMask = red ? artwork.redButtonHover.get() : artwork.silverButtonHover.get();
                if (hoverMask)
                    DrawRectangle(rect, Color(255, 255, 255, 78), *hoverMask);
            }
        }
        drawBoldText(label, rect.GetCenter(), 11.0f * scale,
            red ? Color(255, 255, 255) : Color(31, 31, 29),
            TextAlignmentX::CENTER, TextAlignmentY::CENTER);
    };
    drawDialogButton(controls.confirm, true, "Give a speech");
    drawDialogButton(controls.cancel, false, "Cancel");
    if (previousFont && extras::defaultTextRenderer)
        extras::defaultTextRenderer->setFont(*previousFont);
}

void UpdateSpeechDialog(glm::vec2 mouse, int width, int height) {
    if (IsKeyPressed(GLFW_KEY_ESCAPE)) {
        game.showSpeechDialog = false;
        return;
    }
    if (!IsMouseButtonPressed(GLFW_MOUSE_BUTTON_LEFT)) return;

    const SpeechDialogControls controls = GetSpeechDialogControls(width, height);
    if (Contains(controls.confirm, mouse)) {
        PlayButtonClickSound();
        game.showSpeechDialog = false;
        game.speechUsed = true;
        // Sprite 409's "Give a speech" runs addMoraleForAll(50, human) and
        // leaves the newspaper alone.
        AddMoraleToFaction(game.human, 50);
    } else if (Contains(controls.cancel, mouse)) {
        PlayButtonClickSound();
        game.showSpeechDialog = false;
    }
}

float SelectionFactionCenterX(const SelectionControls& controls, int faction) {
    // sprite_232 frame_010 places the badge clips at x=913, 2467, 4021 and
    // 5574 twips: 77.7 px apart, drawn here 0.65 px left of each origin.
    return controls.offset + (45.0f + faction * 77.7f) * controls.uiScale;
}

Rect SelectionFactionHitRect(const SelectionControls& controls, int faction) {
    const float scale = controls.uiScale;
    const float centerX = SelectionFactionCenterX(controls, faction);
    return Rect(centerX - 28.5f * scale, controls.footerTop + 12.0f * scale,
        57.0f * scale, 69.0f * scale);
}

void DrawSelectionFooter(const SelectionControls& controls, const BoardLayout& layout, int width, int height) {
    const float scale = controls.uiScale;
    // Let the complete map-frame artwork, including its reflection and fade,
    // finish before painting the solid footer backing.
    const Rect mapFrame = GetHexBoardFrameRect(layout);
    const float mapFrameBottom = mapFrame.position.y + mapFrame.size.y;
    const float footerBackgroundTop = std::clamp(std::max(controls.footerTop, mapFrameBottom),
        controls.footerTop, static_cast<float>(height));
    DrawRectangle(Rect(0, footerBackgroundTop, static_cast<float>(width),
        static_cast<float>(height) - footerBackgroundTop), Color(36, 37, 37));

    for (int faction = 0; faction < kFactionCount; ++faction) {
        const float centerX = SelectionFactionCenterX(controls, faction);
        // The whole badge clip (shield and caption) scales around its origin
        // at (45.65 + 77.7 * faction, 563.7): 75% at rest, 100% when hovered,
        // with its alpha following the same value.
        const float visualScale = game.factionShieldScales[static_cast<size_t>(faction)];
        const float groupScale = visualScale;
        const glm::vec2 clipOrigin(centerX + 0.65f * scale, controls.footerTop + 54.7f * scale);
        const Rect flag(clipOrigin + kBadgeShieldOrigin * (scale * visualScale),
            kBadgeShieldSize * (scale * visualScale));
        DrawFactionShield(faction, flag, game.previewFaction == faction, scale, visualScale);
        // Field 97 (name_field) is white Arial Narrow Bold at 12 px, centered
        // 0.6 px left of the clip origin with its baseline 23.312 px below it.
        const int labelAlpha = static_cast<int>(std::lround(255.0f * visualScale));
        if (Text* renderer = extras::defaultTextRenderer) {
            Font* previousFont = renderer->font;
            const float previousStroke = renderer->strokeWidth;
            if (artwork.arialNarrowFont)
                renderer->setFont(*artwork.arialNarrowFont);
            const int previousWeight = renderer->fontWeight;
            renderer->setStrokeWidth(0.0f);
            // Flash's ink for this caption matches weight 400 at the resting
            // 75% scale and weight 470 at full size.
            const float hoverAmount = std::clamp((groupScale - 0.75f) / 0.25f, 0.0f, 1.0f);
            renderer->setFontWeight(static_cast<int>(std::lround(400.0f + 70.0f * hoverAmount)));
            renderer->setHorizontalScale(1.0f);
            renderer->setCharacterSize(12.0f * scale * groupScale);
            renderer->setColor(Color(255, 255, 255, labelAlpha));
            renderer->setString(kFactions[faction]);
            renderer->DrawPrecise(clipOrigin.x - 0.6f * scale * groupScale,
                clipOrigin.y + 23.312f * scale * groupScale,
                TextAlignmentX::CENTER, TextAlignmentY::BASELINE);
            renderer->setFontWeight(previousWeight);
            renderer->setStrokeWidth(previousStroke);
            if (previousFont)
                renderer->setFont(*previousFont);
        }
    }

    if (game.showHelp) return;

    // Shape 104 fills bitmap 103 with matrix (0.9943, 0.0083, -0.0083, 0.9944);
    // the bitmap's center lands at (572.29, 667.46) on the stage.
    const Rect paper(controls.offset + (572.29f - 233.66f) * scale,
        controls.footerTop + (667.46f - 509.0f - 158.6f) * scale,
        467.32f * scale, 317.2f * scale);
    const float paperTilt = std::atan2(0.0083f, 0.9943f);
    if (artwork.parchment) DrawRectangle(paper, paperTilt, *artwork.parchment);
    else DrawRectangle(paper, paperTilt, Color(215, 204, 181));

    Text* renderer = extras::defaultTextRenderer;
    if (!renderer) return;
    // Sprite 115's text is placed in stage coordinates; the footer starts at
    // stage y=509.
    auto drawPaperText = [&](const std::string& text, float stageX, float stageBaselineY,
        float fontSize, TextAlignmentX alignmentX = TextAlignmentX::LEFT) {
        renderer->setCharacterSize(fontSize * scale);
        renderer->setColor(Color(0, 0, 0));
        renderer->setString(text);
        renderer->DrawPrecise(controls.offset + stageX * scale,
            controls.footerTop + (stageBaselineY - 509.0f) * scale,
            alignmentX, TextAlignmentY::BASELINE);
    };

    const int faction = game.previewFaction;
    if (faction < 0) {
        // Static text 106: Adobe Caslon Pro at 25 px, origin (377, 542) with
        // its baseline 25 px down. Flash rasterizes it 1.25 px right of and
        // 0.5 px above that origin.
        // Flash's render also carries about 4% more ink than weight 400.
        const int previousWeight = renderer->fontWeight;
        renderer->setFontWeight(425);
        drawPaperText("Select a capital of your country.", 378.25f, 566.5f, 25.0f);
        renderer->setFontWeight(previousWeight);
        return;
    }

    // Frames 2-5 of sprite 115 hold one static text per nation (107, 112,
    // 113, 114) at (377, 514): the name on a 22 px baseline (Redosia's is
    // 20 px tall) and two 16 px description lines at 44 and 61 px. The
    // wording, typos included, is the SWF's.
    struct NationText { float nameSize; const char* line1; const char* line2; };
    constexpr std::array<NationText, kFactionCount> nationTexts = {{
        { 20.0f, "Thousands years of statehood and vibrant culture",
            "undoubtely justify all the claims of superiority." },
        { 22.0f, "The honest and hard-working folk just cannot stand all those",
            "dirty Reddies, Blueies and Greenies on the face of Earth." },
        { 22.0f, "Cheerful Bluegarians love to sing and dance, they",
            "hate Redosians, Violetnamians and Greenlandians." },
        { 22.0f, "Greenlandians are just like a birds! They need to feel",
            "refreshing breeze of freedom, they need more space." }
    }};
    const NationText& nation = nationTexts[static_cast<size_t>(faction)];
    drawPaperText(kFactions[faction], 377.0f, 536.0f, nation.nameSize);
    drawPaperText(nation.line1, 377.0f, 558.0f, 16.0f);
    drawPaperText(nation.line2, 377.0f, 575.0f, 16.0f);
    // Static text 111 at (377, 581), 12 px.
    drawPaperText("(click to choose this Great Nation)", 377.0f, 593.0f, 12.0f);

    // Static text 109 sits at (520.5, 518.5); edit field 110 (variable
    // "capital") right-aligns 14 px text so its line ends at x=649.
    drawPaperText("capital:", 520.5f, 532.45f, 14.0f);
    const int capital = Index(kCapitals[faction][0], kCapitals[faction][1]);
    const std::string& capitalName = game.tiles[capital].townName;
    constexpr float capitalLineWidth = 81.9f;
    const float capitalNaturalWidth = MeasureText(capitalName, 14.0f).maxWidth;
    // The SWF field would wrap a name wider than its line; keep it on one
    // line by shrinking it instead.
    const float capitalFontSize = capitalNaturalWidth > capitalLineWidth
        ? 14.0f * capitalLineWidth / capitalNaturalWidth : 14.0f;
    drawPaperText(capitalName, 649.0f, 532.48f, capitalFontSize, TextAlignmentX::RIGHT);
    // Shape 108's double rule spans (520.2, 536.7) to (652.8, 540.5).
    if (artwork.capitalNameRule) {
        DrawRectangle(Rect(controls.offset + 520.2f * scale,
            controls.footerTop + (536.7f - 509.0f) * scale, 132.6f * scale, 3.8f * scale),
            *artwork.capitalNameRule);
    }
}

void DrawHelpOverlay(int width, int height, glm::vec2 mouse) {
    DrawRectangle(Rect(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height)),
        Color(0, 0, 0, 140));
    const float helpX = (width - 900.0f) * 0.5f;
    const float helpY = (height - 600.0f) * 0.5f;
    const Rect helpStage(helpX, helpY, 900.0f, 600.0f);
    const size_t page = static_cast<size_t>(std::clamp(game.helpPage, 0, 3));
    if (artwork.helpPages[page])
        DrawRectangle(helpStage, *artwork.helpPages[page]);
    // Sprite 191's capital.gotoAndStop(2): the "(e.g. ...)" pin on page 2.
    if (page == 1 && artwork.tutorialCapitalPin) {
        constexpr float pinSize = 20.1f * 1.343063f;
        DrawRectangle(Rect(helpX + 275.65f - pinSize * 0.5f, helpY + 140.75f - pinSize * 0.5f,
            pinSize, pinSize), *artwork.tutorialCapitalPin);
    }

    const HelpControls helpControls = GetHelpControls(width, height);
    const bool mouseDown = IsMouseButtonDown(GLFW_MOUSE_BUTTON_LEFT);
    auto helpButtonState = [&](Rect rect) {
        if (Contains(rect, mouse) && mouseDown) return ButtonVisualState::Pressed;
        if (Contains(rect, mouse)) return ButtonVisualState::Hovered;
        return ButtonVisualState::Normal;
    };
    DrawPillButton(helpControls.done, "DONE", true,
        helpButtonState(helpControls.done), 1.0f, Color(255, 255, 255, 204));
    if (page == 0) {
        DrawPillButton(helpControls.middle, "Next", false,
            helpButtonState(helpControls.middle), 1.0f, Color(0, 0, 0, 192));
    } else {
        DrawPillButton(helpControls.middle, "Previous", false,
            helpButtonState(helpControls.middle), 1.0f, Color(0, 0, 0, 192));
    }
    if (page > 0 && page + 1 < artwork.helpPages.size()) {
        DrawPillButton(helpControls.next, "Next", false,
            helpButtonState(helpControls.next), 1.0f, Color(0, 0, 0, 192));
    }
}

void DrawSignPactResult(const PactDialogControls& controls, int width, int height, glm::vec2 mouse) {
    const float scale = controls.scale;
    DrawRectangle(Rect(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height)),
        Color(0, 0, 0, 192));
    const bool signedPact = game.pactDialogPage == PactDialogPage::Signed;
    Texture* panel = signedPact ? artwork.pactSignedPanel.get() : artwork.pactRefusedPanel.get();
    // The panel bitmaps start at export pixel (210, 150) and (265, 220).
    const glm::vec2 panelOrigin = signedPact ? glm::vec2(210.0f, 150.0f) : glm::vec2(265.0f, 220.0f);
    if (panel) {
        DrawRectangle(Rect(controls.stage.position.x + panelOrigin.x * scale,
            controls.stage.position.y + panelOrigin.y * scale,
            panel->width * scale, panel->height * scale), *panel);
    }
    const bool hovered = Contains(controls.result, mouse);
    const ButtonVisualState state = !hovered ? ButtonVisualState::Normal
        : IsMouseButtonDown(GLFW_MOUSE_BUTTON_LEFT) ? ButtonVisualState::Pressed : ButtonVisualState::Hovered;
    DrawPillButton(controls.result, signedPact ? "OK" : "Too bad", true, state, scale,
        Color(255, 255, 255, 204), 255, 32.0f);
}

void DrawSignPactDialog(int width, int height, glm::vec2 mouse) {
    const PactDialogControls controls = GetPactDialogControls(width, height);
    if (game.pactDialogPage != PactDialogPage::Choose) {
        DrawSignPactResult(controls, width, height, mouse);
        return;
    }
    const float scale = controls.scale;
    const float stageLeft = std::clamp(controls.stage.position.x, 0.0f, static_cast<float>(width));
    const float stageTop = std::clamp(controls.stage.position.y, 0.0f, static_cast<float>(height));
    const float stageRight = std::clamp(controls.stage.position.x + controls.stage.size.x,
        0.0f, static_cast<float>(width));
    const float stageBottom = std::clamp(controls.stage.position.y + controls.stage.size.y,
        0.0f, static_cast<float>(height));
    // frame_005 already carries the original 75%-opacity black backdrop within
    // its stage; extend that same dimming to any screen area outside the stage.
    const glm::vec4 backdropTint = Color(0, 0, 0, 192);
    if (stageTop > 0.0f)
        DrawRectangle(Rect(0.0f, 0.0f, static_cast<float>(width), stageTop), backdropTint);
    if (stageBottom < height)
        DrawRectangle(Rect(0.0f, stageBottom, static_cast<float>(width), height - stageBottom), backdropTint);
    if (stageLeft > 0.0f && stageBottom > stageTop)
        DrawRectangle(Rect(0.0f, stageTop, stageLeft, stageBottom - stageTop), backdropTint);
    if (stageRight < width && stageBottom > stageTop)
        DrawRectangle(Rect(stageRight, stageTop, width - stageRight, stageBottom - stageTop), backdropTint);

    if (artwork.pactDialog) {
        DrawRectangle(controls.stage, *artwork.pactDialog);
    } else {
        DrawRectangle(controls.stage, Color(235, 235, 216));
        DrawRectangleWires(controls.stage, Color(45, 43, 35));
    }

    // The extracted frame contains placeholders for its four live coat-of-arms
    // clips. Repaint that strip, then draw the faction-specific art and labels.
    DrawRectangle(Rect(controls.stage.position.x + 320.0f * scale,
        controls.stage.position.y + 314.0f * scale, 467.0f * scale, 96.0f * scale),
        Color(235, 235, 216));

    for (int faction = 0; faction < kFactionCount; ++faction) {
        const Rect hit = controls.parties[static_cast<size_t>(faction)];
        const glm::vec2 center = hit.GetCenter();
        const bool available = IsPactPartyAvailable(faction);
        const float opacity = available ? 1.0f : 0.5f;
        const int alpha = static_cast<int>(std::lround(opacity * 255.0f));
        const Rect shield(center.x - 10.5f * scale,
            controls.stage.position.y + 349.0f * scale,
            21.0f * scale, 28.0f * scale);
        const FactionBadgeState badgeState = DrawFactionBadgeArt(faction, shield, false, scale, opacity);
        const glm::vec4 nameColor = available
            ? Color(35, 32, 26, alpha) : Color(136, 136, 122, alpha);
        DrawText(FactionBadgeTitle(badgeState.status) + " of " + kFactions[static_cast<size_t>(faction)],
            glm::vec2(center.x, controls.stage.position.y + 397.0f * scale),
            9.5f * scale, nameColor,
            TextAlignmentX::CENTER, TextAlignmentY::CENTER);
    }

    const bool hovered = Contains(controls.cancel, mouse);
    const bool pressed = hovered && IsMouseButtonDown(GLFW_MOUSE_BUTTON_LEFT);
    if (artwork.silverButton) {
        DrawRectangle(controls.cancel, Color(255, 255, 255), *artwork.silverButton);
        if (pressed) {
            DrawRectangle(controls.cancel, Color(0, 0, 0, 42), *artwork.silverButton);
        } else if (hovered && artwork.silverButtonHover) {
            DrawRectangle(controls.cancel, Color(255, 255, 255, 78), *artwork.silverButtonHover);
        }
    }
    DrawText("Cancel", controls.cancel.GetCenter(), 11.0f * scale,
        Color(37, 34, 29), TextAlignmentX::CENTER, TextAlignmentY::CENTER);
}

void DrawModalDialog(int width, int height, glm::vec2 mouse);

void DrawMenu(int width, int height, glm::vec2 mouse) {
    // Sprite 232 only reaches the capital hints (frame 10) after sprite 192.
    const int timelineFrame = MenuTimelineFrame();
    const bool controlsShown = timelineFrame >= 10;
    if (controlsShown && game.modalDialog == ModalDialogKind::None) game.capitalTooltipTimerStarted = true;
    Font* previousFont = extras::defaultTextRenderer ? extras::defaultTextRenderer->font : nullptr;
    if (artwork.selectionFont && extras::defaultTextRenderer)
        extras::defaultTextRenderer->setFont(*artwork.selectionFont);

    const SelectionControls controls = GetSelectionControls(width, height);
    const float scale = controls.uiScale;
    const BoardLayout layout = GetSelectionBoardLayout(width, height);
    DrawRectangle(Rect(0, 0, static_cast<float>(width), static_cast<float>(height)), Color(27, 29, 29));
    // The board frame's dark gradients extend up behind the control strip;
    // the SWF has no separate header fill.
    DrawHexBoardFrame(layout);
    DrawSelectionMap(layout, width, height, true);

    // Sprite 80 at depth 155 covers map generation: black with the crest and
    // "Unbelievably complex calculations in progress....", fading out by 0.25
    // a frame over frames 3-6 while the board waits at y = 80.
    static constexpr std::array<float, 7> generationScreenAlpha = { 1.0f, 1.0f, 1.0f, 0.75f, 0.5f, 0.25f, 0.0f };
    const float generationAlpha = generationScreenAlpha[static_cast<size_t>(std::min(timelineFrame, 6))];
    if (generationAlpha > 0.0f && artwork.mapGenerationScreen) {
        DrawRectangle(Rect(controls.offset - 7.5f * scale, -54.0f * scale,
            artwork.mapGenerationScreen->width * scale, artwork.mapGenerationScreen->height * scale),
            glm::vec4(1.0f, 1.0f, 1.0f, generationAlpha), *artwork.mapGenerationScreen);
    }

    // Frame 10 places the selection strip and footer; its sprite 43 buttons
    // fade in over their first four frames.
    const float buttonFade = std::clamp(0.25f * static_cast<float>(
        static_cast<int>(game.menuElapsed * 12.0f) - 9), 0.0f, 1.0f);
    const auto fade = [buttonFade](glm::vec4 color) { color.a *= buttonFade; return color; };
    const int plateAlpha = static_cast<int>(std::lround(255.0f * buttonFade));

    Text* labelRenderer = extras::defaultTextRenderer;
    if (labelRenderer && controlsShown) {
        Font* previousMapLabelFont = labelRenderer->font;
        const float previousMapLabelScale = labelRenderer->horizontalScale;
        const int previousMapLabelWeight = labelRenderer->fontWeight;
        if (artwork.mapLabelFont)
            labelRenderer->setFont(*artwork.mapLabelFont);
        labelRenderer->setHorizontalScale(1.0f);
        labelRenderer->setFontWeight(400);
        labelRenderer->setCharacterSize(18.0f * scale);
        labelRenderer->setColor(Color(255, 255, 255));
        // Static text 200 is white Arno Pro Bold Display at 18 px, placed at
        // (438, 11.35) with its baseline 15 px down. Its glyph records carry
        // tighter advances than the font's own metrics: 280, 148, 178, 54 twips.
        // Flash rasterizes it half a pixel right of and above that origin.
        constexpr float labelPenX = 438.5f;
        constexpr float labelBaselineY = 25.85f;
        struct LabelGlyph { const char* glyph; float penX; };
        constexpr LabelGlyph labelGlyphs[] = {
            { "M", 0.0f }, { "a", 14.0f }, { "p", 21.4f }, { "\xE2\x84\x96", 33.0f }
        };
        for (const LabelGlyph& glyph : labelGlyphs) {
            labelRenderer->setString(glyph.glyph);
            labelRenderer->DrawPrecise(
                controls.offset + (labelPenX + glyph.penX + kSelectionStageOffsetX) * scale,
                labelBaselineY * scale,
                TextAlignmentX::LEFT, TextAlignmentY::BASELINE);
        }
        labelRenderer->setFontWeight(previousMapLabelWeight);
        labelRenderer->setHorizontalScale(previousMapLabelScale);
        if (previousMapLabelFont)
            labelRenderer->setFont(*previousMapLabelFont);
    }

    if (controlsShown) {
        DrawPillButton(controls.help, "help", true, SelectionButtonVisualState(0), scale,
            fade(Color(255, 255, 255, 204)), plateAlpha);
        DrawPillButton(controls.easy, "easy", false, SelectionButtonVisualState(1, game.difficulty == 0),
            scale, fade(game.difficulty == 0 ? Color(255, 255, 255, 128) : Color(0, 0, 0, 192)), plateAlpha);
        const bool normalSelected = game.difficulty == 5;
        DrawPillButton(controls.normal, "normal", false, SelectionButtonVisualState(2, normalSelected),
            scale, fade(normalSelected ? Color(255, 255, 255, 128) : Color(0, 0, 0, 192)), plateAlpha);
        DrawPillButton(controls.hard, "hard", false, SelectionButtonVisualState(3, game.difficulty == 10),
            scale, fade(game.difficulty == 10 ? Color(255, 255, 255, 128) : Color(0, 0, 0, 192)), plateAlpha);

        // SWF depth order: outline 136, field plate 137, map field 138, then the
        // label and caption buttons.
        if (artwork.mapControlsOutline)
            DrawRectangle(controls.mapLabel, *artwork.mapControlsOutline);
        ConfigureMapNumberInput(controls);
        DrawPillButton(controls.mapInput, "", false, ButtonVisualState::Normal, scale,
            Color(55, 55, 55), 255);
        TextInputBox& mapInput = *game.mapNumberInput;
        // The box's own text renderer can't rotate and snaps to whole pixels,
        // so it only paints the selection and cursor; the digits always come
        // from DrawMapNumberText so they stay put while highlighted.
        mapInput.textColor = Color(0, 0, 0, 0);
        mapInput.selectedTextColor = Color(0, 0, 0, 0);
        mapInput.Draw();
        DrawMapNumberText(controls);

        DrawPillButton(controls.changeMap, "CHANGE MAP", true, SelectionButtonVisualState(4),
            scale, fade(Color(255, 255, 255, 204)), plateAlpha);
        DrawPillButton(controls.randomMap, "RANDOM MAP", true, SelectionButtonVisualState(5),
            scale, fade(Color(255, 255, 255, 204)), plateAlpha);
    }

    // The board's speaker and Q, then sprite 192 at depth 163 of sprite 232.
    DrawAudioToggle(width, height);
    DrawModalDialog(width, height, mouse);

    // Shape 170's white gradient sits at depth 201, above every control in
    // the strip, which is what gives the plates their glossy top, and above
    // sprite 192's modal, which leaves it undimmed.
    if (artwork.gameplaySkyline) {
        DrawRectangle(Rect(controls.offset, -10.0f * scale, 800.0f * scale, 61.4f * scale),
            *artwork.gameplaySkyline);
    }

    const float previousSelectionStroke = extras::defaultTextRenderer
        ? extras::defaultTextRenderer->strokeWidth : 0.0f;
    if (artwork.selectionFont && extras::defaultTextRenderer)
        extras::defaultTextRenderer->setFont(*artwork.selectionFont);
    if (extras::defaultTextRenderer)
        extras::defaultTextRenderer->setStrokeWidth(0.08f * scale);
    if (controlsShown) DrawSelectionFooter(controls, layout, width, height);

    if (game.showHelp) DrawHelpOverlay(width, height, mouse);

    if (extras::defaultTextRenderer)
        extras::defaultTextRenderer->setStrokeWidth(previousSelectionStroke);
    if (previousFont && extras::defaultTextRenderer)
        extras::defaultTextRenderer->setFont(*previousFont);
}

// Sprite 231 sits at stage (400, 299.95) and shares the board's stage scale.
struct ResultScreenLayout {
    float scale;
    glm::vec2 stageOrigin;
    Rect button;
    Rect stageRect(float x, float y, float w, float h) const {
        return Rect(stageOrigin.x + x * scale, stageOrigin.y + y * scale, w * scale, h * scale);
    }
};

ResultScreenLayout GetResultScreenLayout(int width, int height) {
    const BoardLayout board = GetBoardLayout(width, height);
    ResultScreenLayout layout{ board.scale, glm::vec2(board.left - 17.0f * board.scale, 0.0f), Rect() };
    // Sprite 43 at sprite-local (-287.95, 125.5); its 98x32 plate starts there.
    layout.button = layout.stageRect(112.05f, 425.45f, 98.0f, 32.0f);
    return layout;
}

bool ResultPanelVisible() {
    return ResultTimelineFrame() >= kResultPanelFrame;
}

// Centred single line with Flash letterSpacing added after every glyph.
void DrawSpacedTextLine(const std::string& line, glm::vec2 centerBaseline, float fontSize,
    float letterSpacing, glm::vec4 color) {
    Text* renderer = extras::defaultTextRenderer;
    if (!renderer || line.empty()) return;
    renderer->setCharacterSize(fontSize);
    const float lineWidth = renderer->GetCursorX(line, static_cast<int>(line.size())) +
        letterSpacing * static_cast<float>(line.size());
    const float left = centerBaseline.x - lineWidth * 0.5f;
    for (size_t index = 0; index < line.size(); ++index) {
        renderer->setCharacterSize(fontSize);
        const float x = left + renderer->GetCursorX(line, static_cast<int>(index)) +
            letterSpacing * static_cast<float>(index);
        DrawText(std::string(1, line[index]), glm::vec2(x, centerBaseline.y), fontSize, color,
            TextAlignmentX::LEFT, TextAlignmentY::BASELINE);
    }
}

void DrawResult(int width, int height, glm::vec2 mouse) {
    // The board stays on stage under the modal; the header clips are gone.
    Font* previousFont = extras::defaultTextRenderer ? extras::defaultTextRenderer->font : nullptr;
    if (artwork.selectionFont && extras::defaultTextRenderer)
        extras::defaultTextRenderer->setFont(*artwork.selectionFont);
    const BoardLayout board = GetBoardLayout(width, height);
    DrawHexBoardFrame(board);
    DrawSelectionMap(board, width, height, false, true);
    DrawBoardFrameHelpIcon(width, height, false, BoardFrameFadeAlpha());
    DrawAudioToggle(width, height);

    // Shape 172 is black at alpha 192; frames 1-4 multiply it by 0, .25, .5
    // and .75 before shape 216 holds it at full strength.
    const int frame = ResultTimelineFrame();
    static constexpr std::array<int, kResultPanelFrame> modalAlpha = { 0, 48, 96, 144 };
    const int alpha = frame < kResultPanelFrame ? modalAlpha[static_cast<size_t>(frame)] : 192;
    DrawRectangle(Rect(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height)),
        Color(0, 0, 0, alpha));
    if (frame < kResultPanelFrame) {
        if (previousFont && extras::defaultTextRenderer)
            extras::defaultTextRenderer->setFont(*previousFont);
        return;
    }

    const ResultScreenLayout layout = GetResultScreenLayout(width, height);
    Texture* panel = game.playerWon ? artwork.resultPanelWin.get() : artwork.resultPanelLost.get();
    if (panel) {
        // The panel bitmaps start at export pixel (125, 128): stage (75, 127.95).
        DrawRectangle(layout.stageRect(75.0f, 127.95f,
            static_cast<float>(panel->width), static_cast<float>(panel->height)), *panel);
    }

    // Frames 6/7 set `result` for edit-text fields 218/225: font 174 at 18 px,
    // letterSpacing 0.5, centred. The baselines come from each field's matrix
    // and the font ascent; lines advance by ascent + descent + 2 px leading.
    const std::string seed = MapSeedText();
    const std::string turns = std::to_string(game.turns);
    const std::array<std::string, 2> resultLines = game.playerWon
        ? std::array<std::string, 2>{ "You conquered all the Map Nr " + seed, "in " + turns + " turns." }
        : std::array<std::string, 2>{ "Your Empire on the Map Nr " + seed, "lasted for " + turns + " turns." };
    const glm::vec2 firstBaseline = game.playerWon
        ? glm::vec2(506.7f, 284.911f) : glm::vec2(503.95f, 332.165f);
    constexpr float lineAdvance = (18160.0f + 4020.0f) / 20480.0f * 18.0f + 2.0f;
    if (artwork.resultFont && extras::defaultTextRenderer)
        extras::defaultTextRenderer->setFont(*artwork.resultFont);
    for (size_t line = 0; line < resultLines.size(); ++line) {
        const glm::vec2 baseline = layout.stageOrigin +
            glm::vec2(firstBaseline.x, firstBaseline.y + lineAdvance * static_cast<float>(line)) * layout.scale;
        DrawSpacedTextLine(resultLines[line], baseline, 18.0f * layout.scale,
            0.5f * layout.scale, Color(0, 0, 0));
    }
    if (previousFont && extras::defaultTextRenderer)
        extras::defaultTextRenderer->setFont(*previousFont);

    // Sprite 43 fades its button child in over its first four frames.
    const float buttonOpacity = std::clamp((frame - kResultPanelFrame) * 0.25f, 0.0f, 1.0f);
    const bool hovered = Contains(layout.button, mouse);
    const ButtonVisualState state = !hovered ? ButtonVisualState::Normal
        : IsMouseButtonDown(GLFW_MOUSE_BUTTON_LEFT) ? ButtonVisualState::Pressed : ButtonVisualState::Hovered;
    DrawPillButton(layout.button, game.playerWon ? "I'm so happy" : "Life's tough", true, state,
        layout.scale, Color(255, 255, 255, static_cast<int>(std::lround(204.0f * buttonOpacity))),
        static_cast<int>(std::lround(255.0f * buttonOpacity)), 32.0f);
}

enum class ModalDialogAction { Close, Next, Previous, GiveUp, DeclineTutorial };

struct ModalDialogButton {
    const char* label;
    bool red;
    // Top-left of sprite 43's 98x32 plate in the sprite's 900x600 export.
    glm::vec2 position;
    ModalDialogAction action;
    // True when the page keeps the previous frame's instance, so it does not
    // fade in again.
    bool keepsFade = false;
};

struct ModalDialogPage {
    Texture* panel;
    // Top-left of the panel bitmap in the sprite's 900x600 export.
    glm::vec2 panelOrigin;
    std::array<ModalDialogButton, 2> buttons;
    int buttonCount;
    // Export-pixel centre of the page's sprite 189 pin, which the page sends
    // to its capital frame; the panel bitmap has paper there.
    std::optional<glm::vec2> capitalPin = std::nullopt;
};

// Pages are the sprites' frames 5-7; positions come from their placements
// (sprite-local + 450, 300). Button actions follow each clip's operation.
std::vector<ModalDialogPage> GetModalDialogPages(ModalDialogKind kind) {
    using A = ModalDialogAction;
    switch (kind) {
    case ModalDialogKind::Capitulate:
        return { { artwork.capitulatePanel.get(), { 196.0f, 150.0f }, {{
            { "Give up", true, { 232.0f, 402.5f }, A::GiveUp },
            { "Cancel", false, { 330.0f, 402.5f }, A::Close } }}, 2 } };
    case ModalDialogKind::YourTurn:
        return {
            { artwork.yourTurnPanels[0].get(), { 80.0f, 128.0f }, {{
                { "I get it", true, { 127.1f, 425.5f }, A::Close },
                { "Read more", false, { 225.1f, 425.5f }, A::Next } }}, 2 },
            { artwork.yourTurnPanels[1].get(), { 154.0f, 20.0f }, {{
                { "DONE", true, { 191.1f, 533.5f }, A::Close },
                { "Next", false, { 289.1f, 533.5f }, A::Next } }}, 2 },
            // Frame 7 only replaces depth 46; DONE carries over from frame 6.
            { artwork.yourTurnPanels[2].get(), { 154.0f, 20.0f }, {{
                { "DONE", true, { 191.1f, 533.5f }, A::Close, true },
                { "Previous", false, { 289.1f, 533.5f }, A::Previous } }}, 2 } };
    case ModalDialogKind::Tutorial:
        return {
            { artwork.tutorialPanels[0].get(), { 235.0f, 213.0f }, {{
                { "Yes, please", true, { 272.0f, 340.5f }, A::Next },
                { "No, thanks", false, { 370.0f, 340.5f }, A::DeclineTutorial } }}, 2 },
            { artwork.tutorialPanels[1].get(), { 140.0f, 150.0f }, {{
                { "I get it", true, { 177.1f, 403.5f }, A::Close },
                { "Read more", false, { 275.1f, 403.5f }, A::Next } }}, 2, glm::vec2(595.0f, 275.95f) },
            { artwork.tutorialPanels[2].get(), { 154.0f, 73.0f }, {{
                { "DONE", true, { 191.1f, 496.5f }, A::Close },
                {} }}, 1, glm::vec2(274.65f, 197.75f) } };
    case ModalDialogKind::None:
        break;
    }
    return {};
}

// Like the other modal dialogs, the 800x600 stage is centred in the window.
// The dialog sprites sit at stage (400, 300), so export x - 50 is stage x.
struct ModalDialogLayout {
    float scale;
    glm::vec2 origin;
    Rect exportRect(glm::vec2 position, float w, float h) const {
        return Rect(origin.x + (position.x - 50.0f) * scale, origin.y + position.y * scale,
            w * scale, h * scale);
    }
};

ModalDialogLayout GetModalDialogLayout(int width, int height) {
    const float scale = std::clamp(std::min(width / 800.0f, height / 600.0f), 0.72f, 1.2f);
    return { scale, glm::vec2((width - 800.0f * scale) * 0.5f, (height - 600.0f * scale) * 0.5f) };
}

// Frames 1-4 fade the modal in; the panel arrives on frame 5.
bool ModalDialogPanelVisible() {
    return game.modalDialogElapsed * 12.0f >= kResultPanelFrame;
}

void OpenModalDialog(ModalDialogKind kind) {
    game.modalDialog = kind;
    game.modalDialogPage = 0;
    game.modalDialogElapsed = 0.0f;
    game.modalButtonElapsed.fill(0.0f);
}

void ShowModalDialogPage(int page) {
    const std::vector<ModalDialogPage> pages = GetModalDialogPages(game.modalDialog);
    if (page < 0 || page >= static_cast<int>(pages.size())) return;
    game.modalDialogPage = page;
    for (int index = 0; index < 2; ++index)
        if (!pages[static_cast<size_t>(page)].buttons[static_cast<size_t>(index)].keepsFade)
            game.modalButtonElapsed[static_cast<size_t>(index)] = 0.0f;
}

void CloseModalDialog() {
    const ModalDialogKind closed = game.modalDialog;
    game.modalDialog = ModalDialogKind::None;
    // Sprite 437 stopped h0-h2 while open; its buttons gotoAndPlay(1) them.
    if (closed == ModalDialogKind::YourTurn && game.openingTooltipSecondsRemaining > 0.0f)
        game.openingTooltipSecondsRemaining = kOpeningTooltipDurationSeconds;
}

void UpdateModalDialog(glm::vec2 mouse, int width, int height, float deltaTime) {
    const bool panelWasVisible = ModalDialogPanelVisible();
    game.modalDialogElapsed += std::max(0.0f, deltaTime);
    if (panelWasVisible)
        for (float& elapsed : game.modalButtonElapsed) elapsed += std::max(0.0f, deltaTime);
    if (!panelWasVisible || !IsMouseButtonReleased(GLFW_MOUSE_BUTTON_LEFT)) return;

    const std::vector<ModalDialogPage> pages = GetModalDialogPages(game.modalDialog);
    if (game.modalDialogPage >= static_cast<int>(pages.size())) return;
    const ModalDialogPage& page = pages[static_cast<size_t>(game.modalDialogPage)];
    const ModalDialogLayout layout = GetModalDialogLayout(width, height);
    for (int index = 0; index < page.buttonCount; ++index) {
        const ModalDialogButton& button = page.buttons[static_cast<size_t>(index)];
        if (!Contains(layout.exportRect(button.position, 98.0f, 32.0f), mouse)) continue;
        PlayButtonClickSound();
        switch (button.action) {
        case ModalDialogAction::Close: CloseModalDialog(); break;
        case ModalDialogAction::Next: ShowModalDialogPage(game.modalDialogPage + 1); break;
        case ModalDialogAction::Previous: ShowModalDialogPage(game.modalDialogPage - 1); break;
        case ModalDialogAction::GiveUp:
            // "Give up" plays the game clip on to its game_over frame.
            CloseModalDialog();
            ShowResultScreen(false);
            break;
        case ModalDialogAction::DeclineTutorial:
            // "No, thanks" also skips the "It's your turn!" notice.
            game.yourTurnShown = true;
            CloseModalDialog();
            break;
        }
        return;
    }
}

void DrawModalDialog(int width, int height, glm::vec2 mouse) {
    if (game.modalDialog == ModalDialogKind::None) return;
    const int frame = static_cast<int>(game.modalDialogElapsed * 12.0f);
    static constexpr std::array<int, kResultPanelFrame> modalAlpha = { 0, 48, 96, 144 };
    const int alpha = frame < kResultPanelFrame ? modalAlpha[static_cast<size_t>(frame)] : 192;
    DrawRectangle(Rect(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height)),
        Color(0, 0, 0, alpha));
    if (frame < kResultPanelFrame) return;

    const std::vector<ModalDialogPage> pages = GetModalDialogPages(game.modalDialog);
    if (game.modalDialogPage >= static_cast<int>(pages.size())) return;
    const ModalDialogPage& page = pages[static_cast<size_t>(game.modalDialogPage)];
    const ModalDialogLayout layout = GetModalDialogLayout(width, height);
    if (page.panel) {
        DrawRectangle(layout.exportRect(page.panelOrigin,
            static_cast<float>(page.panel->width), static_cast<float>(page.panel->height)), *page.panel);
    }
    if (page.capitalPin && artwork.tutorialCapitalPin) {
        // Sprite 189 spans 402 twips and is placed at scale 1.343063.
        constexpr float pinSize = 20.1f * 1.343063f;
        DrawRectangle(layout.exportRect(*page.capitalPin - glm::vec2(pinSize * 0.5f),
            pinSize, pinSize), *artwork.tutorialCapitalPin);
    }
    for (int index = 0; index < page.buttonCount; ++index) {
        const ModalDialogButton& button = page.buttons[static_cast<size_t>(index)];
        const Rect rect = layout.exportRect(button.position, 98.0f, 32.0f);
        const float opacity = std::clamp(
            game.modalButtonElapsed[static_cast<size_t>(index)] * 12.0f * 0.25f, 0.0f, 1.0f);
        const bool hovered = Contains(rect, mouse);
        const ButtonVisualState state = !hovered ? ButtonVisualState::Normal
            : IsMouseButtonDown(GLFW_MOUSE_BUTTON_LEFT) ? ButtonVisualState::Pressed : ButtonVisualState::Hovered;
        const glm::vec4 labelColor = button.red ? Color(255, 255, 255, 204) : Color(0, 0, 0, 192);
        DrawPillButton(rect, button.label, button.red, state, layout.scale,
            glm::vec4(labelColor.r, labelColor.g, labelColor.b, labelColor.a * opacity),
            static_cast<int>(std::lround(255.0f * opacity)), 32.0f);
    }
}

void OfferPact(int faction) {
    if (faction == game.human || !IsAlive(faction)) return;
    // signPact: no roll; every kingdom refuses while human_condition > 1.
    // The dialog then plays its "fail" or "success" frame, and frame 7 sets
    // hw_pact_signed. Neither outcome touches the newspaper.
    if (HumanCondition() > 1) {
        game.pactDialogPage = PactDialogPage::Refused;
        return;
    }
    game.pactPartner = faction;
    game.pactSigned = true;
    game.pactDialogPage = PactDialogPage::Signed;
}

void FilterMapNumberInput() {
    if (!game.mapNumberInput) return;
    TextInputBox& input = *game.mapNumberInput;
    const std::string original = input.text;
    const int oldCursor = std::clamp(input.cursorPosition, 0, static_cast<int>(original.size()));
    const int oldSelectionStart = input.selectionStart;
    const int oldSelectionEnd = input.selectionEnd;
    std::string digits;
    digits.reserve(6);

    auto filteredPosition = [&](int position) {
        position = std::clamp(position, 0, static_cast<int>(original.size()));
        int result = 0;
        for (int i = 0; i < position && result < 6; ++i) {
            if (original[static_cast<size_t>(i)] >= '0' &&
                original[static_cast<size_t>(i)] <= '9')
                ++result;
        }
        return result;
    };

    for (char character : original) {
        if (digits.size() == 6) break;
        if (character >= '0' && character <= '9')
            digits.push_back(character);
    }

    input.text = std::move(digits);
    input.cursorPosition = filteredPosition(oldCursor);
    if (oldSelectionStart >= 0 && oldSelectionEnd >= 0) {
        input.selectionStart = filteredPosition(oldSelectionStart);
        input.selectionEnd = filteredPosition(oldSelectionEnd);
    } else {
        input.selectionStart = -1;
        input.selectionEnd = -1;
    }
    input.renderText.setString(input.text);
}

void ApplyMapNumber() {
    if (!game.mapNumberInput) return;
    FilterMapNumberInput();
    game.blankMapNumber = game.mapNumberInput->text.empty();
    if (!game.blankMapNumber) {
        try {
            game.mapNumber = std::clamp(std::stoi(game.mapNumberInput->text), 0, 999999);
        } catch (...) {
            game.mapNumber = 408739;
        }
    }
    game.mapNumberInput->text = MapSeedText();
    game.mapNumberInput->finalText = game.mapNumberInput->text;
    game.mapNumberInput->cursorPosition = static_cast<int>(game.mapNumberInput->text.size());
    game.mapNumberInput->clearSelection();
    game.mapNumberInput->hasFocus = false;
    game.previewFaction = -1;
    GenerateMap(game.mapNumber);
}

void UpdateHelpDialog(glm::vec2 mouse, int width, int height) {
    const HelpControls helpControls = GetHelpControls(width, height);
    if (IsKeyPressed(GLFW_KEY_ESCAPE)) {
        game.showHelp = false;
        game.helpPage = 0;
        return;
    }
    if (IsKeyPressed(GLFW_KEY_LEFT))
        game.helpPage = std::max(0, game.helpPage - 1);
    if (IsKeyPressed(GLFW_KEY_RIGHT))
        game.helpPage = std::min(3, game.helpPage + 1);

    if (!IsMouseButtonReleased(GLFW_MOUSE_BUTTON_LEFT)) return;
    if (Contains(helpControls.done, mouse)) {
        PlayButtonClickSound();
        game.showHelp = false;
        game.helpPage = 0;
    } else if (game.helpPage == 0 && Contains(helpControls.middle, mouse)) {
        PlayButtonClickSound();
        ++game.helpPage;
    } else if (game.helpPage > 0 && Contains(helpControls.middle, mouse)) {
        PlayButtonClickSound();
        --game.helpPage;
    } else if (game.helpPage > 0 && game.helpPage < 3 &&
        Contains(helpControls.next, mouse)) {
        PlayButtonClickSound();
        ++game.helpPage;
    }
}

// Sprite 264 frames that place its children, whose own timelines then loop.
constexpr int kIntroPhotoFrame = 17;
constexpr int kIntroTextFrame = 30;
constexpr int kIntroStartFrame = 37;
constexpr int kIntroHoldFrame = 42;

Rect IntroStageRect(int width, int height, float x, float y, float w, float h) {
    const float scale = SelectionUiScale(width, height);
    const float offset = SelectionUiOffset(width, scale);
    return Rect(offset + x * scale, y * scale, w * scale, h * scale);
}

Rect IntroStartButtonRect(int width, int height) {
    const IntroLayerFrame& button = kIntroStartButton[kIntroHoldFrame - 1];
    return IntroStageRect(width, height, button.x, button.y, 98.0f, 32.0f);
}

// One 12 fps frame of sprite 264 and its children.
void StepIntroFrame() {
    ++game.introTicks;
    if (game.introFrame == kIntroHoldFrame && !game.introStartPressed) {
        // Frame 42's stop() holds the timeline; the clips on it keep running.
    } else if (game.introFrame < kIntroFrames) {
        ++game.introFrame;
        if (game.introFrame == 2 && gameAudio.introMusic) gameAudio.introMusic->play();
    }
    if (game.introFrame == kIntroPhotoFrame && game.introTicks == kIntroPhotoFrame - 1) {
        // onClipEvent(load) for foto_2 (d = -7) and foto_1 (d = 7).
        for (size_t index = 0; index < game.introPhotos.size(); ++index) {
            GameState::IntroPhoto& photo = game.introPhotos[index];
            photo.startX = kIntroPhoto2[kIntroPhotoFrame - 1].x;
            photo.x = photo.startX;
            photo.step = index == 0 ? -7.0f : 7.0f;
            photo.alpha = 0;
            photo.photo = EffectsRandom(static_cast<int>(artwork.introPhotos.size()));
        }
    }
    if (game.introTicks >= kIntroPhotoFrame - 1) {
        // Sprite 254's enterFrame: drift with a little jitter while fading in,
        // and start over with another photo after 500 px.
        for (GameState::IntroPhoto& photo : game.introPhotos) {
            photo.x += photo.step + EffectsRandom(4001) / 1000.0f - 2.0f;
            photo.alpha = std::min(100, photo.alpha + 5);
            if (std::abs(photo.x - photo.startX) > 500.0f) {
                photo.x = photo.startX;
                photo.alpha = 0;
                photo.photo = EffectsRandom(static_cast<int>(artwork.introPhotos.size()));
            }
        }
    }
}

void FinishIntro() {
    // Frame 47 stops sound 252 and runs _root.gotoAndPlay("game_start").
    if (gameAudio.introMusic) gameAudio.introMusic->stop();
    game.screen = Screen::Menu;
    game.menuElapsed = 0.0f;
    StartMenuMusic();
}

void UpdateIntro(glm::vec2 mouse, int width, int height, float deltaTime) {
    // Flash shows every frame, so a long frame only advances one.
    game.introFrameTime += std::clamp(deltaTime, 0.0f, 1.0f / 12.0f);
    while (game.introFrameTime >= 1.0f / 12.0f) {
        game.introFrameTime -= 1.0f / 12.0f;
        StepIntroFrame();
    }
    if (game.introFrame >= kIntroFrames) {
        FinishIntro();
        return;
    }
    // START (sprite 43 from frame 37) runs button._parent.play(), which only
    // matters once frame 42 has stopped the timeline.
    if (game.introFrame >= kIntroStartFrame && IsMouseButtonReleased(GLFW_MOUSE_BUTTON_LEFT) &&
        Contains(IntroStartButtonRect(width, height), mouse)) {
        PlayButtonClickSound();
        if (game.introFrame == kIntroHoldFrame) game.introStartPressed = true;
    }
}

void DrawIntro(int width, int height, glm::vec2 mouse) {
    DrawRectangle(Rect(0, 0, static_cast<float>(width), static_cast<float>(height)), Color(0, 0, 0));
    const size_t frame = static_cast<size_t>(std::clamp(game.introFrame, 1, kIntroFrames) - 1);
    const auto draw = [&](Texture* texture, const IntroLayerFrame& row, float alpha,
        glm::vec2 offset = glm::vec2(0.0f), float scale = 1.0f, float angle = 0.0f) {
        if (!texture || !row.present || alpha <= 0.0f) return;
        DrawRectangle(IntroStageRect(width, height, row.x + offset.x, row.y + offset.y,
            texture->width * scale, texture->height * scale), angle,
            glm::vec4(1.0f, 1.0f, 1.0f, alpha), *texture);
    };
    const auto age = [](int placedFrame) { return std::max(0, game.introTicks - (placedFrame - 1)); };

    draw(artwork.introBackdrop.get(), kIntroBackdrop[frame], kIntroBackdrop[frame].alpha);
    // Depths 2 and 4: the drifting photos sit right above the backdrop.
    for (size_t index = 0; index < game.introPhotos.size(); ++index) {
        const IntroLayerFrame& row = index == 0 ? kIntroPhoto2[frame] : kIntroPhoto1[frame];
        const GameState::IntroPhoto& photo = game.introPhotos[index];
        draw(artwork.introPhotos[static_cast<size_t>(photo.photo)].get(), row, photo.alpha / 100.0f,
            glm::vec2(photo.x - photo.startX, 0.0f));
    }
    draw(artwork.introBanner.get(), kIntroBanner[frame], kIntroBanner[frame].alpha);

    // Depth 7, sprite 259: its lettering masks a tinted glint (sprite 256)
    // and a turning fire glow (sprite 258), both looping over 100 frames.
    const IntroLayerFrame& text = kIntroText[frame];
    const size_t textLoop = static_cast<size_t>(age(kIntroTextFrame) % kIntroTextLoopFrames);
    draw(artwork.introText[textLoop].get(), text, text.alpha, kIntroTextBarOffset);

    // Depth 14, sprite 78 loops its 13-frame glow pulse; its blur filter was
    // baked per frame by the generator.
    const IntroLayerFrame& crest = kIntroCrest[frame];
    Texture* blurred = nullptr;
    float blurMargin = 0.0f;
    for (size_t index = 0; index < std::size(kIntroBlurredCrests); ++index) {
        if (kIntroBlurredCrests[index].frame == static_cast<int>(frame) + 1) {
            blurred = artwork.introBlurredCrest[index].get();
            blurMargin = kIntroBlurredCrests[index].margin;
        }
    }
    if (blurred)
        draw(blurred, crest, crest.alpha, glm::vec2(-blurMargin));
    else
        draw(artwork.introCrest[static_cast<size_t>(age(kIntroCrestFirstFrame) % 13)].get(), crest, crest.alpha);

    // Depth 19: START, whose sprite 43 also fades its button in over four frames.
    const IntroLayerFrame& start = kIntroStartButton[frame];
    if (start.present) {
        const float buttonFade = std::clamp(0.25f * static_cast<float>(age(kIntroStartFrame)), 0.0f, 1.0f);
        const float opacity = start.alpha * buttonFade;
        const Rect rect = IntroStageRect(width, height, start.x, start.y, 98.0f, 32.0f);
        const bool hovered = Contains(rect, mouse);
        const ButtonVisualState state = !hovered ? ButtonVisualState::Normal
            : IsMouseButtonDown(GLFW_MOUSE_BUTTON_LEFT) ? ButtonVisualState::Pressed : ButtonVisualState::Hovered;
        DrawPillButton(rect, "START", true, state, SelectionUiScale(width, height),
            Color(255, 255, 255, static_cast<int>(std::lround(204.0f * opacity))),
            static_cast<int>(std::lround(255.0f * opacity)), 32.0f);
    }
    draw(artwork.introSponsorLogo.get(), kIntroSponsorLogo[frame], kIntroSponsorLogo[frame].alpha);
    draw(artwork.introSpiral.get(), kIntroSpiral[frame], kIntroSpiral[frame].alpha, glm::vec2(0.0f), 0.283f);
    draw(artwork.introMetaSauce.get(), kIntroMetaSauce[frame], kIntroMetaSauce[frame].alpha, kIntroMetaSauceOffset);
    draw(artwork.introMoreGames.get(), kIntroMoreGames[frame], kIntroMoreGames[frame].alpha);
    draw(artwork.introSkyline.get(), kIntroSkyline[frame], kIntroSkyline[frame].alpha);
    draw(artwork.introVignette.get(), kIntroVignette[frame], kIntroVignette[frame].alpha);
    draw(artwork.introAppStore.get(), kIntroAppStore[frame], kIntroAppStore[frame].alpha);
}

// Picking a capital plays sprite 232 on from frame 10: frame 11 removes the
// selection controls while the board slides back down.
void BeginPreparing(int faction) {
    game.screen = Screen::Preparing;
    game.preparingElapsed = 0.0f;
    game.preparingFaction = faction;
    game.pleaseWaitElapsed = -1.0f;
    game.selectedFaction = faction;
    game.previewFaction = faction;
    game.hoveredTile = -1;
    if (game.mapNumberInput) game.mapNumberInput->hasFocus = false;
}

// Sprite 207 fades its modal in over frames 1-4. From frame 5 its enterFrame
// runs calcAIHelpers for one map row per frame (11 rows for each of the 4
// parties) and the 44th call plays sprite 232 on to frame 14.
constexpr int kPleaseWaitRows = kMapHeight * kFactionCount;
constexpr int kPleaseWaitPanelFrame = 5;
// Summed 1/12 s steps drift below whole frames in float; compare with slack.
constexpr float kFrameSlack = 1e-3f;

int PleaseWaitFrame() {
    return 1 + static_cast<int>(game.pleaseWaitElapsed * 12.0f);
}

void UpdatePreparing(float deltaTime) {
    const float step = std::clamp(deltaTime, 0.0f, 1.0f / 12.0f);
    if (game.pleaseWaitElapsed >= 0.0f && PreparingTimelineFrame() == kPleaseWaitHoldFrame) {
        game.pleaseWaitElapsed += step;
        if (game.pleaseWaitElapsed * 12.0f >= kPleaseWaitPanelFrame - 1 + kPleaseWaitRows - kFrameSlack)
            game.preparingElapsed = 3.0f / 12.0f;
        return;
    }
    game.preparingElapsed += step;
    // Frame 13 attaches sprite 207 and stops.
    if (game.pleaseWaitElapsed < 0.0f && game.preparingElapsed * 12.0f >= 2.0f - kFrameSlack) {
        game.preparingElapsed = 2.0f / 12.0f;
        game.pleaseWaitElapsed = 0.0f;
        return;
    }
    // Frame 20 places the header and runs board.gotoAndPlay("start_game").
    if (PreparingTimelineFrame() >= kCampaignStartFrame)
        StartCampaign(game.preparingFaction);
}

void DrawPleaseWait(int width, int height) {
    // Frame 15 removes sprite 207.
    if (game.pleaseWaitElapsed < 0.0f || PreparingTimelineFrame() > 14) return;
    const int frame = PleaseWaitFrame();
    static constexpr std::array<int, kResultPanelFrame> modalAlpha = { 0, 48, 96, 144 };
    const int alpha = frame <= kResultPanelFrame ? modalAlpha[static_cast<size_t>(frame - 1)] : 192;
    DrawRectangle(Rect(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height)),
        Color(0, 0, 0, alpha));
    if (frame < kPleaseWaitPanelFrame) return;
    const ModalDialogLayout layout = GetModalDialogLayout(width, height);
    if (Texture* panel = artwork.pleaseWaitPanel.get()) {
        // The panel bitmap starts at export pixel (196, 150).
        DrawRectangle(layout.exportRect(glm::vec2(196.0f, 150.0f),
            static_cast<float>(panel->width), static_cast<float>(panel->height)), *panel);
    }
    // gauge.gotoAndStop(ceil(row / 44 * 100)) runs before each row's calcAIHelpers.
    const int call = std::min(kPleaseWaitRows, frame - kPleaseWaitPanelFrame + 1);
    const int gaugeFrame = std::clamp(static_cast<int>(std::ceil((call - 1) * 100.0f / kPleaseWaitRows)), 1, 100);
    if (Texture* gauge = artwork.loadingGauge[static_cast<size_t>(gaugeFrame - 1)].get()) {
        // Sprite 206 at (10, 74) in sprite 207; its outline starts 1 px up-left.
        DrawRectangle(layout.exportRect(glm::vec2(459.0f, 373.0f),
            static_cast<float>(gauge->width), static_cast<float>(gauge->height)), *gauge);
    }
}

void DrawPreparing(int width, int height) {
    Font* previousFont = extras::defaultTextRenderer ? extras::defaultTextRenderer->font : nullptr;
    if (artwork.selectionFont && extras::defaultTextRenderer)
        extras::defaultTextRenderer->setFont(*artwork.selectionFont);
    DrawRectangle(Rect(0, 0, static_cast<float>(width), static_cast<float>(height)), Color(27, 29, 29));
    const BoardLayout layout = GetBoardLayout(width, height);
    DrawHexBoardFrame(layout);
    // The capital fields keep their selection highlight until sprite 81 starts.
    DrawSelectionMap(layout, width, height, true);
    DrawAudioToggle(width, height);
    DrawPleaseWait(width, height);
    DrawGameplaySkyline(width, height);
    if (previousFont && extras::defaultTextRenderer)
        extras::defaultTextRenderer->setFont(*previousFont);
}

void UpdateMenu(glm::vec2 mouse, int width, int height) {
    if (game.showHelp) {
        UpdateHelpDialog(mouse, width, height);
        return;
    }

    const SelectionControls controls = GetSelectionControls(width, height);
    UpdateSelectionButtons(controls, mouse);
    const BoardLayout layout = GetSelectionBoardLayout(width, height);
    game.hoveredTile = FindTileAt(mouse, layout.scale, layout.left, layout.top);

    game.previewFaction = -1;
    for (int faction = 0; faction < kFactionCount; ++faction) {
        const Rect hit = SelectionFactionHitRect(controls, faction);
        if (Contains(hit, mouse)) game.previewFaction = faction;
    }
    if (game.previewFaction < 0 && game.hoveredTile >= 0) {
        for (int faction = 0; faction < kFactionCount; ++faction) {
            if (game.hoveredTile == Index(kCapitals[faction][0], kCapitals[faction][1])) {
                game.previewFaction = faction;
                break;
            }
        }
    }

    const float shieldFrameTime = std::clamp(static_cast<float>(GetFrameTime()), 0.0f, 0.1f);
    // The SWF halves the remaining distance every 12 fps frame; this is the
    // same curve evaluated continuously.
    const float shieldEasing = 1.0f - std::exp2(-12.0f * shieldFrameTime);
    for (int faction = 0; faction < kFactionCount; ++faction) {
        const float targetScale = game.previewFaction == faction ? 1.0f : 0.75f;
        float& currentScale = game.factionShieldScales[static_cast<size_t>(faction)];
        if (currentScale != targetScale) {
            currentScale += (targetScale - currentScale) * shieldEasing;
            if (std::abs(targetScale - currentScale) * 100.0f < 0.8f)
                currentScale = targetScale;
        }
    }

    ConfigureMapNumberInput(controls);
    TextInputBox& mapNumberInput = *game.mapNumberInput;
    const bool mapNumberWasFocused = mapNumberInput.hasFocus;
    mapNumberInput.Update(mouse);
    FilterMapNumberInput();
    if (mapNumberWasFocused && mapNumberInput.escapeCharPressed) {
        ApplyMapNumber();
        return;
    }
    if (mapNumberWasFocused && IsKeyPressed(GLFW_KEY_ESCAPE)) {
        mapNumberInput.hasFocus = false;
    } else if (!mapNumberWasFocused && IsKeyPressed(GLFW_KEY_ENTER)) {
        BeginPreparing(game.previewFaction >= 0 ? game.previewFaction : game.selectedFaction);
        return;
    }
    ConfigureMapNumberInput(controls);

    // The Flash button components dispatch their actions on release.
    const bool mouseReleased = IsMouseButtonReleased(GLFW_MOUSE_BUTTON_LEFT);
    if (game.selectionButtons[0]->WasPressed()) {
        PlayButtonClickSound();
        game.showHelp = true;
        game.helpPage = 0;
        if (game.mapNumberInput) game.mapNumberInput->hasFocus = false;
        return;
    }
    if (game.selectionButtons[1]->WasPressed()) { PlayButtonClickSound(); game.difficulty = 0; return; }
    if (game.selectionButtons[2]->WasPressed()) { PlayButtonClickSound(); game.difficulty = 5; return; }
    if (game.selectionButtons[3]->WasPressed()) { PlayButtonClickSound(); game.difficulty = 10; return; }
    if (mouseReleased && Contains(controls.mapInput, mouse)) {
        if (game.mapNumberInput) game.mapNumberInput->hasFocus = true;
        return;
    }
    if (game.selectionButtons[4]->WasPressed()) { PlayButtonClickSound(); ApplyMapNumber(); return; }
    if (game.selectionButtons[5]->WasPressed()) {
        PlayButtonClickSound();
        game.blankMapNumber = false;
        game.mapNumber = RandomInt(0, 999998);
        if (game.mapNumberInput) {
            game.mapNumberInput->text = std::to_string(game.mapNumber);
            game.mapNumberInput->finalText = game.mapNumberInput->text;
            game.mapNumberInput->cursorPosition = static_cast<int>(game.mapNumberInput->text.size());
            game.mapNumberInput->clearSelection();
            game.mapNumberInput->hasFocus = false;
        }
        game.previewFaction = -1;
        GenerateMap(game.mapNumber);
        return;
    }

    if (!mouseReleased) return;

    for (int faction = 0; faction < kFactionCount; ++faction) {
        const Rect hit = SelectionFactionHitRect(controls, faction);
        if (Contains(hit, mouse)) {
            PlayButtonClickSound();
            BeginPreparing(faction);
            return;
        }
    }

    if (game.hoveredTile >= 0) {
        for (int faction = 0; faction < kFactionCount; ++faction) {
            if (game.hoveredTile == Index(kCapitals[faction][0], kCapitals[faction][1])) {
                PlayButtonClickSound();
                BeginPreparing(faction);
                return;
            }
        }
    }
}

void UpdatePlaying(glm::vec2 mouse, int width, int height) {
    const BoardLayout layout = GetBoardLayout(width, height);
    const GameplayControls controls = GetGameplayControls(width, height);
    UpdateGameplayActionButtons(controls, mouse);
    if (game.choosingPact) {
        if (IsKeyPressed(GLFW_KEY_ESCAPE)) {
            game.choosingPact = false;
            return;
        }
        if (IsMouseButtonPressed(GLFW_MOUSE_BUTTON_LEFT)) {
            const PactDialogControls pactControls = GetPactDialogControls(width, height);
            if (game.pactDialogPage != PactDialogPage::Choose) {
                // "Too bad" / "OK" resume the board and remove the dialog.
                if (Contains(pactControls.result, mouse)) {
                    PlayButtonClickSound();
                    game.choosingPact = false;
                }
                return;
            }
            if (Contains(pactControls.cancel, mouse)) {
                PlayButtonClickSound();
                game.choosingPact = false;
                return;
            }
            for (int faction = 0; faction < kFactionCount; ++faction) {
                if (IsPactPartyAvailable(faction) &&
                    Contains(pactControls.parties[static_cast<size_t>(faction)], mouse)) {
                    PlayButtonClickSound();
                    OfferPact(faction);
                    return;
                }
            }
        }
        return;
    }
    if (game.showSpeechDialog) {
        UpdateSpeechDialog(mouse, width, height);
        return;
    }
    if (game.showHelp) {
        UpdateHelpDialog(mouse, width, height);
        return;
    }
    game.hoveredTile = FindTileAt(mouse, layout.scale, layout.left, layout.top);
    if (IsKeyPressed(GLFW_KEY_ESCAPE)) {
        game.screen = Screen::Menu;
        game.selectedTile = -1;
        game.moveTargets.clear();
        game.previewFaction = -1;
        GenerateMap(game.mapNumber);
        return;
    }
    if (IsMouseButtonReleased(GLFW_MOUSE_BUTTON_LEFT) && Contains(controls.help, mouse)) {
        PlayButtonClickSound();
        game.helpHintVisible = false;
        game.showHelp = true;
        game.helpPage = 0;
        return;
    }
    if (game.currentPlayer != game.human) return;
    {
        if (IsKeyPressed(GLFW_KEY_SPACE)) {
            game.specialButtonsHintVisible = false;
            game.movesLeftHintVisible = false;
            EndHumanTurn();
            return;
        }
        const auto& buttons = game.gameplayActionButtons;
        // Buttons are drawn from top to bottom; reverse order gives overlapping
        // hit areas the same priority as the visible button on top.
        if (buttons[3]->WasPressed()) {
            PlayButtonClickSound();
            game.specialButtonsHintVisible = false;
            // capitulate attaches sprite 407 and stops the board until answered.
            OpenModalDialog(ModalDialogKind::Capitulate);
            return;
        }
        if (buttons[2]->WasPressed()) {
            if (game.pactButtonInactive) return;
            PlayButtonClickSound();
            game.specialButtonsHintVisible = false;
            game.choosingPact = true;
            game.pactDialogPage = PactDialogPage::Choose;
            return;
        }
        // An inactive sprite 42 shows a plain shape with no hit area, so it
        // neither clicks nor opens anything.
        if (buttons[1]->WasPressed()) {
            if (game.speechUsed) return;
            PlayButtonClickSound();
            game.specialButtonsHintVisible = false;
            game.showSpeechDialog = true;
            return;
        }
        if (buttons[0]->WasPressed()) {
            PlayButtonClickSound();
            game.specialButtonsHintVisible = false;
            game.movesLeftHintVisible = false;
            EndHumanTurn();
            return;
        }
        for (const auto& button : buttons) {
            if (button->state == Button::States::HELD ||
                button->state == Button::States::WAS_HELD)
                return;
        }
    }
    if (!IsMouseButtonPressed(GLFW_MOUSE_BUTTON_LEFT)) return;

    const int clicked = FindTileAt(mouse, layout.scale, layout.left, layout.top);
    if (clicked < 0 || game.currentPlayer != game.human) {
        game.selectedTile = -1;
        game.moveTargets.clear();
        return;
    }
    if (game.selectedTile >= 0 && std::find(game.moveTargets.begin(), game.moveTargets.end(), clicked) != game.moveTargets.end()) {
        if (game.armyMoveHintElapsed >= 0.0f && game.selectedTile == game.armyHintTile)
            game.armyHintTile = clicked;
        if (!OrderPlayerMove(game.selectedTile, clicked)) return;
        // The field button (sprite 274) plays s_blop for a move order,
        // s_blop for a selection and s_bup when a selection is dropped.
        PlayButtonClickSound();
        game.selectedTile = -1;
        game.moveTargets.clear();
        game.movesLeftHintVisible = false;
        return;
    }
    if (game.selectedTile == clicked) {
        PlayFieldDeselectSound();
        game.selectedTile = -1;
        game.moveTargets.clear();
    } else if (CanSelectPlayerArmy(clicked)) {
        PlayButtonClickSound();
        if (game.armySelectionHintVisible && clicked == game.armyHintTile) {
            game.armyMoveHintElapsed = 0.0f;
            game.movesLeftHintVisible = false;
            game.specialButtonsHintVisible = false;
            game.helpHintVisible = false;
        }
        game.armySelectionHintVisible = false;
        game.selectedTile = clicked;
        game.moveTargets = PossibleMoves(clicked);
    } else {
        if (game.selectedTile >= 0) PlayFieldDeselectSound();
        game.selectedTile = -1;
        game.moveTargets.clear();
    }
}
}

void InitGame() {
    game = GameState{};
    LoadGameArtwork();
    LoadGameAudio();
    GenerateMap(game.mapNumber);
    // The SWF opens on its title intro before the first country selection.
    game.screen = Screen::Intro;
}

void ShutdownGame() {
    if (extras::defaultTextRenderer && extras::defaultFont)
        extras::defaultTextRenderer->setFont(*extras::defaultFont);
    artwork = GameArtwork{};
}

void UpdateGame(float deltaTime) {
    const int width = std::max(1, GetScreenWidth());
    const int height = std::max(1, GetScreenHeight());
    const glm::vec2 normalized = GetMousePosition();
    const glm::vec2 mouse(normalized.x * width, normalized.y * height);
    if (game.screen == Screen::Intro) {
        UpdateIntro(mouse, width, height, deltaTime);
        return;
    }
    if (UpdateAudioToggle(mouse, width, height)) {
        UpdateGameplayMusic(deltaTime);
        return;
    }
    if (game.screen == Screen::Menu && game.capitalTooltipTimerStarted) {
        game.capitalTooltipSecondsRemaining = std::max(0.0f,
            game.capitalTooltipSecondsRemaining - std::max(0.0f, deltaTime));
    }
    if ((game.screen == Screen::Playing || game.screen == Screen::Result) &&
        game.armyMoveHintElapsed >= 0.0f) {
        game.armyMoveHintElapsed += std::max(0.0f, deltaTime);
        if (game.armyMoveHintElapsed * 12.0f >= 38.0f)
            game.armyMoveHintElapsed = -1.0f;
    }
    // Sprite 437 holds h0-h2 on their current frame until it is dismissed.
    if (game.screen == Screen::Playing && game.openingTooltipSecondsRemaining > 0.0f &&
        game.modalDialog != ModalDialogKind::YourTurn) {
        game.openingTooltipSecondsRemaining = std::max(0.0f,
            game.openingTooltipSecondsRemaining - std::max(0.0f, deltaTime));
        if (game.openingTooltipSecondsRemaining <= 0.0f) {
            game.movesLeftHintVisible = false;
            game.specialButtonsHintVisible = false;
            game.helpHintVisible = false;
        }
    }
    // Sprite 232 plays frames 1-10 from "generate"; on the first visit its
    // frame 6 shows sprite 192 and stops there until it is dismissed.
    // Sprite 81 frame 5 shows sprite 437 on the human's first turn.
    if (game.screen == Screen::Menu && game.modalDialog != ModalDialogKind::Tutorial) {
        // Flash shows every timeline frame, so a long frame (the first one
        // after loading) only advances one 12 fps frame.
        game.menuElapsed += std::clamp(deltaTime, 0.0f, 1.0f / 12.0f);
        if (!game.tutorialPromptShown && game.menuElapsed * 12.0f >= 5.0f) {
            game.menuElapsed = 5.0f / 12.0f;
            game.tutorialPromptShown = true;
            OpenModalDialog(ModalDialogKind::Tutorial);
        }
    }
    if (game.screen == Screen::Playing && !game.yourTurnShown &&
        game.currentPlayer == game.human && game.modalDialog == ModalDialogKind::None) {
        game.yourTurnShown = true;
        OpenModalDialog(ModalDialogKind::YourTurn);
    }
    const bool modalOpen = game.modalDialog != ModalDialogKind::None;
    if (modalOpen) UpdateModalDialog(mouse, width, height, deltaTime);
    if (game.screen == Screen::Menu) {
        // The selection controls and capital fields arrive on frame 10.
        if (!modalOpen && MenuTimelineFrame() >= 10) UpdateMenu(mouse, width, height);
    }
    else if (game.screen == Screen::Preparing) {
        UpdatePreparing(deltaTime);
    }
    else if (game.screen == Screen::Playing) {
        game.gameplayElapsed += std::max(0.0f, deltaTime);
        UpdateBattleExplosions(deltaTime);
        UpdateArmyBlinks(deltaTime);
        // The modal clip covers the board, so it takes all input while open.
        if (!modalOpen) UpdatePlaying(mouse, width, height);
        UpdateTurnPlayback(deltaTime);
        if (game.screen == Screen::Playing && !game.showHelp) UpdateTerritoryFade(deltaTime);
    }
    else if (game.screen == Screen::Result) {
        // Only the board's own clips keep animating under the modal.
        game.resultElapsed += std::max(0.0f, deltaTime);
        UpdateBattleExplosions(deltaTime);
        UpdateArmyBlinks(deltaTime);
        // The panel's button runs button._parent._parent.play(), leaving the game.
        if (ResultPanelVisible() && IsMouseButtonReleased(GLFW_MOUSE_BUTTON_LEFT) &&
            Contains(GetResultScreenLayout(width, height).button, mouse)) {
            PlayButtonClickSound();
            game.screen = Screen::Menu;
            game.selectedFaction = game.human;
            game.previewFaction = -1;
            GenerateMap(game.mapNumber);
        }
    }
    UpdateGameplayMusic(deltaTime);
}

void DrawProgramBezel(int width, int height) {
    if (artwork.gameplayStageBorder) {
        constexpr float stageBorderInset = 1.0f;
        DrawRectangle(Rect(stageBorderInset, stageBorderInset,
            std::max(0.0f, static_cast<float>(width) - stageBorderInset * 2.0f),
            std::max(0.0f, static_cast<float>(height) - stageBorderInset * 2.0f)),
            *artwork.gameplayStageBorder);
        return;
    }

    const float thickness = std::max(1.0f, SelectionUiScale(width, height));
    const glm::vec4 bezelColor = Color(64, 64, 64);
    const float screenWidth = static_cast<float>(width);
    const float screenHeight = static_cast<float>(height);
    DrawRectangle(Rect(0.0f, 0.0f, screenWidth, thickness), bezelColor);
    DrawRectangle(Rect(0.0f, screenHeight - thickness, screenWidth, thickness), bezelColor);
    DrawRectangle(Rect(0.0f, thickness, thickness, screenHeight - thickness * 2.0f), bezelColor);
    DrawRectangle(Rect(screenWidth - thickness, thickness, thickness,
        screenHeight - thickness * 2.0f), bezelColor);
}

void DrawGame() {
    const int width = std::max(1, GetScreenWidth());
    const int height = std::max(1, GetScreenHeight());
    const glm::vec2 normalized = GetMousePosition();
    const glm::vec2 mouse(normalized.x * width, normalized.y * height);
    ClearBackground(Color(18, 28, 33));
    if (game.screen == Screen::Intro) {
        DrawIntro(width, height, mouse);
        DrawProgramBezel(width, height);
        return;
    }
    if (game.screen == Screen::Menu) {
        DrawMenu(width, height, mouse);
    } else if (game.screen == Screen::Preparing) {
        DrawPreparing(width, height);
    } else if (game.screen == Screen::Playing) {
        Font* previousFont = extras::defaultTextRenderer ? extras::defaultTextRenderer->font : nullptr;
        if (artwork.selectionFont && extras::defaultTextRenderer)
            extras::defaultTextRenderer->setFont(*artwork.selectionFont);
        const BoardLayout layout = GetBoardLayout(width, height);
        DrawHexBoardFrame(layout);
        DrawSelectionMap(layout, width, height, false);
        DrawGameplayHeader(width, height, mouse);
        DrawOpeningGameplayTooltips(width, height);
        DrawAudioToggle(width, height);
        // Shape 170's gradient sits at depth 201 in gameplay too, above the
        // badges, buttons and opening tooltips.
        DrawGameplaySkyline(width, height);
        if (game.showSpeechDialog) DrawSpeechDialog(width, height, mouse);
        else if (game.showHelp) DrawHelpOverlay(width, height, mouse);
        else if (game.choosingPact) DrawSignPactDialog(width, height, mouse);
        if (previousFont && extras::defaultTextRenderer)
            extras::defaultTextRenderer->setFont(*previousFont);
    } else {
        DrawResult(width, height, mouse);
        DrawGameplaySkyline(width, height);
    }
    // The menu draws sprite 192 under shape 170 itself; the gameplay dialogs
    // are root clips above the game.
    if (game.screen != Screen::Menu) DrawModalDialog(width, height, mouse);
    DrawProgramBezel(width, height);
}
