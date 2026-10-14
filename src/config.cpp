#include "common.h"
#include "config.h"
#include <stdio.h>
#include <stdlib.h>

static Config g_cfg;

const Config& Cfg() { return g_cfg; }

static const char kDefaultIni[] =
    "; RE3Controls for Resident Evil 5 - settings (1 = on, 0 = off)\r\n"
    "[Controls]\r\n"
    "Enabled=1\r\n"
    "; Mode: Full = everything below; MeleeCamera = the original game, except that the camera stays free and\r\n"
    "; turns with the right stick during melee attacks (of [Camera] it uses Speed, PitchSpeed, InvertY,\r\n"
    "; Distance, Height, Shoulder, FollowPlatform and GrabCamera; AimDistance, FieldOfView and AimFieldOfView\r\n"
    "; stay the game's)\r\n"
    "Mode=Full\r\n"
    "; 1 = the RE3 layout on the gamepad: A = interact (the game's \"X\" prompts show and take A; objects that keep\r\n"
    "; checking the button after their prompt - held switches, co-op \"push together\" - also accept A held), X = reload,\r\n"
    "; Back = map, L3 = run, R3 = camera reset, RB = locate partner (swaps the shoulder while aiming), LT = aim,\r\n"
    "; LB = knife, RT = the game's fire button everywhere (in the aim, and outside it, e.g. Wesker's Knee Cannon);\r\n"
    "; while aiming A is the game's own reload (LT + A) unless a prompt that takes A is on screen.\r\n"
    "; 0 = the original buttons of control type D (X answers the prompts, A runs and reloads; set OneButtonReload=0\r\n"
    "; too, or X also reloads). Default: 1\r\n"
    "RemapButtons=1\r\n"
    "; Run: Toggle (click L3 once), Hold (hold L3) or Stock (hold A, as in the original game). It runs while standing,\r\n"
    "; walking, running, turning or lowering the gun/knife; in cover, on ladders, in attacks, QTEs or Wesker's dash it\r\n"
    "; does nothing (it never reloads in cover or turns the dash). Without the free camera (FreeCamera=0, split-screen\r\n"
    "; player 2) in control types C/D Toggle and Hold give the game its own L3 run: the stick to the side runs and the\r\n"
    "; right stick steers the run, as in the original (Run=Stock runs on A, steered with the left stick).\r\n"
    "; Default: Toggle\r\n"
    "Run=Toggle\r\n"
    "; RunButton: L3 or RB (with RemapButtons=1). RB swaps the two buttons: RB runs (and switches the walking\r\n"
    "; pace while aiming), L3 locates the partner (and swaps the shoulder while aiming)\r\n"
    "RunButton=L3\r\n"
    "; InteractButton (with RemapButtons=1, gamepad): A = the prompts (open, pick up, finishers) show and take A, as\r\n"
    "; in RE3; X = they stay on X, as in the original game: X answers a prompt when one is on screen and reloads\r\n"
    "; otherwise, and A has no use outside the aim (LT + A still reloads). QTEs keep their own buttons with both. The\r\n"
    "; keyboard always uses the game's own action key. Other values count as A. Default: A\r\n"
    "InteractButton=A\r\n"
    "; 1 = X reloads (keyboard: the game's reload key) from standing, walking, running, cover or the aim, without\r\n"
    "; raising the gun. While a prompt or QTE that X answers is on screen, X only answers it; a press is ignored when the\r\n"
    "; game cannot reload the gun in hand (full magazine, no spare ammo), so it never holds back RT.\r\n"
    "; 0 = X does not reload. With both values LT + A reloads while aiming, as in the original game. Default: 1\r\n"
    "OneButtonReload=1\r\n"
    "; 1 = B with the left stick pulled back (toward the camera with FreeCamera=1) turns you round (keyboard: the\r\n"
    "; game's quick turn key), while standing, walking, running or lowering the gun or knife. In cover, on ladders, in\r\n"
    "; attacks, grabs, or when a prompt takes B, B stays the partner button. Keep B held after the turn to give a\r\n"
    "; partner order with the d-pad (releasing it then gives no \"Come on\"). 0 = no quick turn (B is only the partner\r\n"
    "; button; the original stick back + A turn works only with FreeCamera=0 and Run=Stock or RemapButtons=0).\r\n"
    "; Default: 1\r\n"
    "QuickTurn=1\r\n"
    "; Wesker's dash (The Mercenaries). 1 = as in the original game: R3 starts the dash when you are not aiming (it\r\n"
    "; also puts the camera behind Wesker; each dash costs a little health); during the dash the right stick steers,\r\n"
    "; A + the right stick left/right/back turns, RT is the Knee Cannon, and the game's own camera follows the dash.\r\n"
    "; Keyboard/mouse: the game's keys (Left Shift + X starts it, Shift + A/D/S turns, the mouse steers, the left mouse\r\n"
    "; button is the Knee Cannon). With Mode=MeleeCamera the right stick steers the dash too.\r\n"
    "; 0 = as in version 1.8: for Wesker R3 only puts the camera behind him (the gamepad cannot dash) and during a dash\r\n"
    "; the right stick/mouse only turn the camera (no steering, no turns; RT still is the Knee Cannon).\r\n"
    "; Other characters are not affected. Default: 1\r\n"
    "WeskerDash=1\r\n"
    "; the original game's stick-click moves on the gamepad. 1 = hold R3 and click L3 to taunt (any character, also\r\n"
    "; while aiming; that L3 click then does not run, change the walking pace, locate the partner or swap the\r\n"
    "; shoulder), and R3 while aiming goes to the game (the RPG-7 NVS scope switches its night vision). Outside the aim\r\n"
    "; R3 still puts the camera behind you. 0 = as in version 1.8: L3 and R3 are only the mod's buttons (no taunt on the\r\n"
    "; gamepad; Wesker's R3 dash is set by WeskerDash). The keyboard's own taunt key works with both values.\r\n"
    "; Default: 1\r\n"
    "StickClicks=1\r\n"
    "; LB (knife) pressed while LT (aim) is held. 1 = as in the original game: the knife comes out straight from the gun\r\n"
    "; aim (RT slashes) and releasing LB goes back to the gun aim; LT pressed while the knife is out still switches to\r\n"
    "; the gun. 0 = as in version 1.8: LB does nothing while LT is held (release LT first). Keyboard/mouse: the same for\r\n"
    "; the knife key and the right mouse button. Default: 1\r\n"
    "KnifeFromAim=1\r\n"
    "; 1 = walk with the left stick (and run with L3) while the inventory is open; its cursor then moves with the\r\n"
    "; d-pad only. Keyboard: the run key runs when you press it while moving; pressed while standing it confirms in the\r\n"
    "; inventory, as in the original game. 0 = the original: the character stops and the stick moves the cursor.\r\n"
    "; Default: 1\r\n"
    "InventoryMove=1\r\n"
    "\r\n"
    "[Camera]\r\n"
    "; 1 = the right stick turns the camera around Chris/Sheva and the left stick moves relative to the camera. Raising\r\n"
    "; the gun or knife from standing, walking, running or lowering the gun first turns the character to where the\r\n"
    "; camera looks; in cover, on ladders, in attacks, QTEs and Wesker's dash the game aims as it faces.\r\n"
    "; 0 = the game's own camera (the stick moves relative to the character). Default: 1\r\n"
    "FreeCamera=1\r\n"
    "Speed=180\r\n"
    "PitchSpeed=90\r\n"
    "InvertY=0\r\n"
    "; camera like RE3: distance behind the shoulder, pivot height, shift over the shoulder (cm),\r\n"
    "; default tilt in degrees (+ = looking down); R3 puts the camera back behind and at this tilt\r\n"
    "Distance=230\r\n"
    "Height=150\r\n"
    "Shoulder=45\r\n"
    "Tilt=8\r\n"
    "; aiming: how much farther back the camera sits, in cm. It moves straight back along its own view, so the\r\n"
    "; crosshair and where the shots land stay the same, and walls still push it in as in the game. 0 = the game's\r\n"
    "; own aim camera, 50 = half a metre farther back, up to 200 (more counts as 200; negative values or text = 0).\r\n"
    "; Gamepad and keyboard/mouse; not in the scope view (sniper rifles, rocket launchers), on gun emplacements and\r\n"
    "; vehicle guns, or with Mode=MeleeCamera. Default: 0\r\n"
    "AimDistance=0\r\n"
    "; field of view in degrees, added to the game's own (45 outside the aim): 0 = as in the game, 15 = 60 degrees\r\n"
    "; (wider), -10 = 35 (narrower); -15 to 40, beyond that counts as the nearest end, text = 0. Not with\r\n"
    "; Mode=MeleeCamera. Default: 0\r\n"
    "FieldOfView=0\r\n"
    "; the same while aiming (the game's 33 to 40 degrees, by weapon): 0 = as in the game, 10 = 10 degrees wider;\r\n"
    "; -15 to 40. Not in the scope view (sniper rifles, rocket launchers), on gun emplacements and vehicle guns, or\r\n"
    "; with Mode=MeleeCamera. Default: 0\r\n"
    "AimFieldOfView=0\r\n"
    "TurnSpeed=720\r\n"
    "; moving toward the camera (left stick pulled back, S on the keyboard) without running, with FreeCamera=1:\r\n"
    "; 0 = turn round and walk toward the camera, as in RE3 Remake (and RE2/RE4 Remake);\r\n"
    "; 1 = step back while facing away from the camera, as in the original game (the character faces where the camera\r\n"
    "; looks, also while reloading or switching weapons on the move and with the inventory open; running toward the\r\n"
    "; camera still turns round). Sideways and forward are the same with both. Default: 0\r\n"
    "BackStep=0\r\n"
    "; on a turning boat or moving platform the camera turns with it, as the game's own camera does\r\n"
    "FollowPlatform=1\r\n"
    "; melee attacks (A on a staggered enemy, finishers, kicks, stomps): 1 = no cinematic attack camera, the\r\n"
    "; camera stays where you put it and the right stick keeps turning it (needs FreeCamera=1);\r\n"
    "; 0 = the game's attack camera\r\n"
    "MeleeCamera=0\r\n"
    "; enemy grabs (an enemy holds you while you shake free, or until the partner frees you): 1 = no cinematic grab\r\n"
    "; camera, the camera stays where you put it and the right stick (keyboard: the mouse) keeps turning it (needs\r\n"
    "; FreeCamera=1; also works with Mode=MeleeCamera); 0 = the game's grab camera. With both values the game films\r\n"
    "; kills (chainsaw, Executioner and other deadly grabs), story boss scenes, doors, ladders, the fall after a grab\r\n"
    "; and the dying state. Default: 0\r\n"
    "GrabCamera=0\r\n"
    "; crouched behind low cover, with FreeCamera=1. 1 = as in the original game (control type D): the right stick\r\n"
    "; (keyboard: the mouse) also turns the crouching character left/right while it turns the camera.\r\n"
    "; 0 = as in version 1.8: there it only turns the camera. High cover is the same with both values. Default: 1\r\n"
    "CoverTurn=1\r\n"
    "\r\n"
    "[Aim]\r\n"
    "; walk while aiming a gun / with the knife readied / while reloading / while switching weapons (1 = walk;\r\n"
    "; 0 = stand still as in the original game; the camera stays the mod's either way). Default: 1 for all four\r\n"
    "GunWalk=1\r\n"
    "KnifeWalk=1\r\n"
    "ReloadWalk=1\r\n"
    "SwitchWalk=1\r\n"
    "; walk while looking through a scope (sniper rifles, rocket launchers). 1 = the left stick (keyboard: WASD) walks\r\n"
    "; in the scope; to zoom hold RB (L3 with RunButton=RB) and push the left stick up / down - the legs stop while\r\n"
    "; it is held. Keyboard: the mouse wheel zooms, as in the game. Only for control types B and D on the gamepad (in\r\n"
    "; A and C the left stick aims in the scope). 0 = stand still in the scope, the left stick zooms, as in the\r\n"
    "; original game. Default: 0\r\n"
    "ScopeWalk=0\r\n"
    "; walking speed while aiming or holding the knife, 1.0 = the game's normal walk; the run button (L3 or RB)\r\n"
    "; while aiming switches to FastWalkSpeed\r\n"
    "WalkSpeed=1.125\r\n"
    "; fast walk (run button while aiming)\r\n"
    "FastWalkSpeed=1.875\r\n"
    "; knife attack speed (1.0 = the original game)\r\n"
    "KnifeSpeed=1.5\r\n"
    "; a neat crosshair instead of the laser sight (gamepad, and keyboard/mouse with MouseAim=RE3)\r\n"
    "Crosshair=1\r\n"
    "; size of that crosshair: 1.0 = the normal size, 0.5 = half, 2.0 = twice as big; 0.25 to 4 (smaller values\r\n"
    "; count as 0.25, larger ones as 4; 0, a negative number or text = 1.0, so 0 does not hide it: Crosshair=0\r\n"
    "; gives the laser). Its opening on a shot (CrosshairKick) grows and shrinks with it; shots still land under its\r\n"
    "; centre.\r\n"
    "; Only the mod's crosshair changes: the laser, the sniper scope and the game's own reticles (the keyboard's small\r\n"
    "; knife reticle) keep their size. Default: 1.0\r\n"
    "CrosshairSize=1.0\r\n"
    "; horizontal crosshair aim turns the view and the body at once (stick or mouse); 0 = original arm-only\r\n"
    "; aim zone\r\n"
    "DirectHorizontal=1\r\n"
    "; gamepad aim speed, horizontal / vertical, on top of the game's own Aim Speed option: 1.0 = unchanged,\r\n"
    "; 2.0 = twice as fast, 0.5 = half (0.25 to 4). Also in the sniper scope (the game still slows the aim with\r\n"
    "; the zoom). The free camera has its own Speed / PitchSpeed in [Camera]\r\n"
    "AimSpeedX=1.0\r\n"
    "AimSpeedY=1.0\r\n"
    "; gamepad (crosshair or laser): how far the camera trails the aim; 1 = the original pad aim camera\r\n"
    "; (about 0.14 s behind, the character can drift into the crosshair when you turn fast), 0 = no delay;\r\n"
    "; 0.15 is close to the game's own mouse aim camera\r\n"
    "AimCameraLag=0.15\r\n"
    "; how far the crosshair opens on each shot and springs back: 1.0 = the normal opening, 2.0 = twice as far,\r\n"
    "; 0.5 = half; 0 = it never opens, also not the game's own opening for shotguns and magnums (a negative number or\r\n"
    "; text counts as 0). With values above 0 shotguns and magnums still open it as in the game. Only the mod's\r\n"
    "; crosshair (Crosshair=1). Default: 1.0\r\n"
    "CrosshairKick=1.0\r\n"
    "; keyboard/mouse: the game's own small reticle while the knife is out (1); 0 = hidden, as with the gamepad\r\n"
    "KnifeCrosshair=1\r\n"
    "; aim sway (breathing): 0 = a steady aim with the gamepad and the mouse, 1 = the original game's\r\n"
    "Breathing=0\r\n"
    "; automatic weapons kick the aim up a little on each shot, like with a mouse\r\n"
    "Recoil=1\r\n"
    "\r\n"
    "[Damage]\r\n"
    "; 0 = the original game's damage (it scales with how well you play). Any other value is a fixed\r\n"
    "; multiplier for all damage the players deal instead: 3 = three handgun headshots kill a common Majini,\r\n"
    "; like RE3; 1.4 = about the original's start on Normal; 1.0 is a little weaker than the original\r\n"
    "; both co-op players need the mod; the host's values are used\r\n"
    "Damage=0\r\n"
    "; extra multiplier for the knife / machete, on top of Damage (only when Damage is not 0)\r\n"
    "KnifeDamage=2.0\r\n"
    "; rate of fire of all guns: 1.0 = the original game, 2.0 = twice as fast (0.5 to 3); the knife and grenades\r\n"
    "; are not affected; in co-op the host's value is used\r\n"
    "FireRate=1.0\r\n"
    "; shotgun pellet spread: 1.0 = the original game, 0.5 = half as wide, 0 = all pellets in one line (very\r\n"
    "; strong at range); all shotguns of all players; in co-op the host's value is used\r\n"
    "ShotgunSpread=1.0\r\n"
    "\r\n"
    "[Movement]\r\n"
    "; walking and running speed outside the aim: 1.0 = the original game (0.5 to 2); the walking speed while\r\n"
    "; aiming is WalkSpeed / FastWalkSpeed in [Aim]\r\n"
    "MoveWalkSpeed=1.0\r\n"
    "MoveRunSpeed=1.0\r\n"
    "\r\n"
    "[Visual]\r\n"
    "; 1 = remove the yellow colour tint of the levels (the game's own Special Settings filters still work)\r\n"
    "NoColorFilter=0\r\n"
    "\r\n"
    "[Advanced]\r\n"
    "; milliseconds the mod waits after the game window appears before it hooks the game (raise it if the game\r\n"
    "; says \"Program integrity check failed\" at start)\r\n"
    "HookDelayMs=3000\r\n"
    "\r\n"
    "[Keyboard]\r\n"
    "; 1 = the mod's controls also on keyboard and mouse: the mouse turns the camera, WASD move relative to it, the\r\n"
    "; reload key reloads on the move, the quick turn key turns around, walking while aiming works. The keys are the\r\n"
    "; game's own (Options > Keyboard/mouse; defaults WASD, Shift run, C quick turn, RMB aim, Space knife, LMB fire,\r\n"
    "; R reload, F action, V partner, Q locate partner / swap shoulder while aiming, M map, E inventory); the middle\r\n"
    "; mouse button puts the camera behind you. With FreeCamera=0 the game's own look left/right keys keep turning the\r\n"
    "; character. 0 = the game's own keyboard/mouse controls. Default: 1\r\n"
    "KeyboardMouse=1\r\n"
    "; running on the keyboard: Hold (hold the run key) or Toggle (press it once)\r\n"
    "Run=Hold\r\n"
    "; mouse speed for the camera and the aim, on top of the game's own mouse sensitivity option (1.0 = the\r\n"
    "; game's speed; the camera turns exactly as far per mouse movement as the aim does); vertical relative to it\r\n"
    "MouseSensitivity=0.75\r\n"
    "MouseVertical=1.0\r\n"
    "; aiming: RE3 = the crosshair stays in the centre and the mouse turns the aim (the game's mouse-look style);\r\n"
    "; Game = the aim style set in the game's options\r\n"
    "MouseAim=RE3\r\n"
    "; a gamepad player who touches the mouse switches to keyboard/mouse only after this many mouse counts within\r\n"
    "; about a quarter of a second (0 = the game's own rule: any movement). Keys and mouse buttons switch at once\r\n"
    "MouseSwitchCounts=30\r\n"
    "\r\n"
    "[Coop]\r\n"
    "; Extra position updates so the partner sees free-camera turns and walking while aiming\r\n"
    "Sync=1\r\n";

static bool GetBool(const char* path, const char* sec, const char* key, bool def) {
    return GetPrivateProfileIntA(sec, key, def ? 1 : 0, path) != 0;
}

static float GetFloat(const char* path, const char* sec, const char* key, float def) {
    char buf[64], dflt[64];
    sprintf(dflt, "%g", def);
    GetPrivateProfileStringA(sec, key, dflt, buf, sizeof(buf), path);
    return (float)atof(buf);
}

// A whole number like "0.5" or "0,5" (spaces around allowed); false for empty or other text.
static bool ParseNumber(char* buf, float* out) {
    for (char* p = buf; *p; ++p)
        if (*p == ',') *p = '.';
    char* end = nullptr;
    const double v = strtod(buf, &end);
    if (end == buf) return false;
    while (*end == ' ' || *end == '\t') ++end;
    if (*end) return false;
    *out = (float)v;
    return v == v;
}

// A speed multiplier: "1,5" works like "1.5"; anything that is not a positive number means the default.
static float GetScale(const char* path, const char* sec, const char* key, float lo, float hi, float def = 1.0f) {
    char buf[64], dflt[32];
    sprintf(dflt, "%g", def);
    GetPrivateProfileStringA(sec, key, dflt, buf, sizeof(buf), path);
    for (char* p = buf; *p; ++p)
        if (*p == ',') *p = '.';
    const float v = (float)atof(buf);
    if (!(v > 0.0f)) return def;  // also NaN
    return v < lo ? lo : v > hi ? hi : v;
}

static FILETIME g_iniTime = {};

// Re-read the ini when it changes on disk (cheap: one attribute query per second).
void PollConfig() {
    static DWORD last = 0;
    DWORD now = GetTickCount();
    if (now - last < 1000) return;
    last = now;
    char path[MAX_PATH];
    wsprintfA(path, "%sRE3Controls.ini", ModDir());
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &a)) return;
    if (CompareFileTime(&a.ftLastWriteTime, &g_iniTime) != 0) LoadConfig();
}

void LoadConfig() {
    char path[MAX_PATH];
    wsprintfA(path, "%sRE3Controls.ini", ModDir());
    if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) {
        HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            DWORD w;
            WriteFile(h, kDefaultIni, sizeof(kDefaultIni) - 1, &w, nullptr);
            CloseHandle(h);
        }
    }
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (GetFileAttributesExA(path, GetFileExInfoStandard, &fa)) g_iniTime = fa.ftLastWriteTime;
    // The profile API keeps the last ini file it read cached; drop it so edits made while the game runs apply.
    WritePrivateProfileStringA(nullptr, nullptr, nullptr, path);
    static bool logged = false;
    if (!logged) {
        Log("config file: %s", path);
        logged = true;
    }
    Config c;
    c.enabled = GetBool(path, "Controls", "Enabled", c.enabled);
    c.remapButtons = GetBool(path, "Controls", "RemapButtons", c.remapButtons);
    char run[32];
    GetPrivateProfileStringA("Controls", "Run", "Toggle", run, sizeof(run), path);
    c.runMode = !_stricmp(run, "Hold") ? RunMode::Hold : !_stricmp(run, "Stock") ? RunMode::Stock : RunMode::Toggle;
    char runButton[32];
    GetPrivateProfileStringA("Controls", "RunButton", "L3", runButton, sizeof(runButton), path);
    c.runButton = !_stricmp(runButton, "RB") ? RunButton::RB : RunButton::L3;
    c.oneButtonReload = GetBool(path, "Controls", "OneButtonReload", c.oneButtonReload);
    c.quickTurn = GetBool(path, "Controls", "QuickTurn", c.quickTurn);
    c.weskerDash = GetBool(path, "Controls", "WeskerDash", c.weskerDash);
    c.stickClicks = GetBool(path, "Controls", "StickClicks", c.stickClicks);
    c.knifeFromAim = GetBool(path, "Controls", "KnifeFromAim", c.knifeFromAim);
    c.inventoryMove = GetBool(path, "Controls", "InventoryMove", c.inventoryMove);
    c.freeCamera = GetBool(path, "Camera", "FreeCamera", c.freeCamera);
    c.cameraSpeed = GetFloat(path, "Camera", "Speed", c.cameraSpeed);
    c.cameraPitchSpeed = GetFloat(path, "Camera", "PitchSpeed", c.cameraPitchSpeed);
    c.invertCameraY = GetBool(path, "Camera", "InvertY", c.invertCameraY);
    c.cameraDistance = GetFloat(path, "Camera", "Distance", c.cameraDistance);
    c.cameraHeight = GetFloat(path, "Camera", "Height", c.cameraHeight);
    c.cameraShoulder = GetFloat(path, "Camera", "Shoulder", c.cameraShoulder);
    c.cameraDefaultPitch = GetFloat(path, "Camera", "Tilt", c.cameraDefaultPitch);
    {
        char buf[64];
        GetPrivateProfileStringA("Camera", "AimDistance", "0", buf, sizeof(buf), path);
        float v = 0.0f;
        c.aimDistance = ParseNumber(buf, &v) && v > 0.0f ? (v > 200.0f ? 200.0f : v) : 0.0f;
    }
    {
        auto degrees = [&](const char* key) {
            char buf[64];
            GetPrivateProfileStringA("Camera", key, "0", buf, sizeof(buf), path);
            float v = 0.0f;
            if (!ParseNumber(buf, &v) || !(v == v)) return 0.0f;
            return v < -15.0f ? -15.0f : v > 40.0f ? 40.0f : v;
        };
        c.fieldOfView = degrees("FieldOfView");
        c.aimFieldOfView = degrees("AimFieldOfView");
    }
    c.turnSpeed = GetFloat(path, "Camera", "TurnSpeed", c.turnSpeed);
    c.backStep = GetBool(path, "Camera", "BackStep", c.backStep);
    c.coverTurn = GetBool(path, "Camera", "CoverTurn", c.coverTurn);
    c.followPlatform = GetBool(path, "Camera", "FollowPlatform", c.followPlatform);
    c.meleeCamera = GetBool(path, "Camera", "MeleeCamera", c.meleeCamera);
    c.grabCamera = GetBool(path, "Camera", "GrabCamera", c.grabCamera);
    // AimWalk (up to v1.4) is the default for the three separate switches.
    const bool aimWalk = GetBool(path, "Aim", "AimWalk", true);
    c.gunWalk = GetBool(path, "Aim", "GunWalk", aimWalk);
    c.knifeWalk = GetBool(path, "Aim", "KnifeWalk", aimWalk);
    c.reloadWalk = GetBool(path, "Aim", "ReloadWalk", aimWalk);
    c.switchWalk = GetBool(path, "Aim", "SwitchWalk", aimWalk);
    c.scopeWalk = GetBool(path, "Aim", "ScopeWalk", c.scopeWalk);
    {
        char ib[16];
        GetPrivateProfileStringA("Controls", "InteractButton", "A", ib, sizeof(ib), path);
        c.interactX = !_stricmp(ib, "X");
    }
    c.aimWalkAnimSpeed = GetFloat(path, "Aim", "WalkSpeed", c.aimWalkAnimSpeed);
    c.aimWalkFastSpeed = GetFloat(path, "Aim", "FastWalkSpeed", c.aimWalkFastSpeed);
    c.crosshair = GetBool(path, "Aim", "Crosshair", c.crosshair);
    c.directHorizontal = GetBool(path, "Aim", "DirectHorizontal", c.directHorizontal);
    c.aimSpeedX = GetScale(path, "Aim", "AimSpeedX", 0.25f, 4.0f);
    c.aimSpeedY = GetScale(path, "Aim", "AimSpeedY", 0.25f, 4.0f);
    {
        char buf[64];
        GetPrivateProfileStringA("Aim", "AimCameraLag", "0.15", buf, sizeof(buf), path);
        float v = 0.15f;
        c.aimCameraLag = ParseNumber(buf, &v) ? (v >= 0.0f && v <= 1.0f ? v : (v > 1.0f ? 1.0f : 0.15f)) : 0.15f;
    }
    {
        const int ms = (int)GetPrivateProfileIntA("Advanced", "HookDelayMs", 3000, path);
        c.hookDelayMs = (unsigned)(ms < 0 ? 0 : ms > 60000 ? 60000 : ms);
    }
    c.crosshairSize = GetScale(path, "Aim", "CrosshairSize", 0.25f, 4.0f);
    c.crosshairKick = GetFloat(path, "Aim", "CrosshairKick", c.crosshairKick);
    c.knifeCrosshair = GetBool(path, "Aim", "KnifeCrosshair", c.knifeCrosshair);
    c.knifeSpeed = GetFloat(path, "Aim", "KnifeSpeed", c.knifeSpeed);
    c.breathing = GetBool(path, "Aim", "Breathing", c.breathing);
    c.padRecoil = GetBool(path, "Aim", "Recoil", c.padRecoil);
    c.damage = GetFloat(path, "Damage", "Damage", c.damage);
    c.knifeDamage = GetFloat(path, "Damage", "KnifeDamage", c.knifeDamage);
    c.fireRate = GetScale(path, "Damage", "FireRate", 0.5f, 3.0f);
    {
        char buf[64];
        GetPrivateProfileStringA("Damage", "ShotgunSpread", "1", buf, sizeof(buf), path);
        float v = 1.0f;
        c.shotgunSpread = ParseNumber(buf, &v) && v >= 0.0f && v <= 1.0f ? v : 1.0f;  // anything else: the original
    }
    c.moveWalk = GetScale(path, "Movement", "MoveWalkSpeed", 0.5f, 2.0f);
    c.moveRun = GetScale(path, "Movement", "MoveRunSpeed", 0.5f, 2.0f);
    c.noColorFilter = GetBool(path, "Visual", "NoColorFilter", c.noColorFilter);
    c.coopSync = GetBool(path, "Coop", "Sync", c.coopSync);
    c.keyboardMouse = GetBool(path, "Keyboard", "KeyboardMouse", c.keyboardMouse);
    char kbRun[32];
    GetPrivateProfileStringA("Keyboard", "Run", "Hold", kbRun, sizeof(kbRun), path);
    c.kbRunMode = !_stricmp(kbRun, "Toggle") ? RunMode::Toggle : RunMode::Hold;
    c.mouseSensitivity = GetScale(path, "Keyboard", "MouseSensitivity", 0.1f, 10.0f, 0.75f);
    c.mouseVertical = GetScale(path, "Keyboard", "MouseVertical", 0.1f, 10.0f);
    char mouseAim[32];
    GetPrivateProfileStringA("Keyboard", "MouseAim", "RE3", mouseAim, sizeof(mouseAim), path);
    c.mouseAimRE3 = _stricmp(mouseAim, "Game") != 0;
    {
        const int n = GetPrivateProfileIntA("Keyboard", "MouseSwitchCounts", c.mouseSwitchCounts, path);
        c.mouseSwitchCounts = n < 0 ? 0 : n > 100000 ? 100000 : n;
    }
    char mode[32];
    GetPrivateProfileStringA("Controls", "Mode", "Full", mode, sizeof(mode), path);
    if (!_stricmp(mode, "MeleeCamera")) {
        // Melee camera only: the original game everywhere else (the camera placement settings still apply).
        c.meleeOnly = true;
        c.meleeCamera = true;
        c.remapButtons = false;
        c.runMode = RunMode::Stock;
        c.runButton = RunButton::L3;
        c.oneButtonReload = false;
        c.quickTurn = false;
        c.stickClicks = false;
        c.knifeFromAim = false;
        c.inventoryMove = false;
        c.freeCamera = false;
        c.gunWalk = c.knifeWalk = c.reloadWalk = c.switchWalk = c.scopeWalk = false;
        c.knifeSpeed = 1.0f;
        c.crosshair = false;
        c.directHorizontal = false;
        c.aimSpeedX = c.aimSpeedY = 1.0f;
        c.aimCameraLag = 1.0f;
        c.breathing = true;
        c.padRecoil = false;
        c.damage = 0.0f;
        c.knifeDamage = 1.0f;
        c.fireRate = c.moveWalk = c.moveRun = 1.0f;
        c.shotgunSpread = 1.0f;
        c.aimDistance = c.fieldOfView = c.aimFieldOfView = 0.0f;
        Log("config: Mode=MeleeCamera (everything else as in the original game)");
    }
    g_cfg = c;
    Log("config: remap=%d run=%s/%s reload=%d quickturn=%d invmove=%d freecam=%d (%.0f/%.0f) aimwalk=%d (%.2f/%.2f) crosshair=%d (%.2f) "
        "aimspeed=%.2f/%.2f aimlag=%.2f fire=%.2f spread=%.2f move=%.2f/%.2f nofilter=%d sync=%d kbm=%d (%s, %.2f/%.2f, %s, %d) "
        "dash=%d clicks=%d knifeaim=%d coverturn=%d backstep=%d aimdist=%.0f fov=%+.0f/%+.0f grabcam=%d scopewalk=%d interact=%s",
        c.remapButtons, run, c.runButton == RunButton::RB ? "RB" : "L3", c.oneButtonReload, c.quickTurn, c.inventoryMove, c.freeCamera, c.cameraSpeed, c.cameraPitchSpeed,
        c.gunWalk + c.knifeWalk * 2 + c.reloadWalk * 4 + c.switchWalk * 8, c.aimWalkAnimSpeed, c.aimWalkFastSpeed,
        c.crosshair, c.crosshairSize, c.aimSpeedX, c.aimSpeedY, c.aimCameraLag, c.fireRate, c.shotgunSpread, c.moveWalk, c.moveRun, c.noColorFilter,
        c.coopSync, c.keyboardMouse, c.kbRunMode == RunMode::Hold ? "hold" : "toggle", c.mouseSensitivity,
        c.mouseVertical, c.mouseAimRE3 ? "RE3" : "game", c.mouseSwitchCounts, c.weskerDash, c.stickClicks, c.knifeFromAim,
        c.coverTurn, c.backStep, c.aimDistance, c.fieldOfView, c.aimFieldOfView, c.grabCamera, c.scopeWalk,
        c.interactX ? "X" : "A");
}
