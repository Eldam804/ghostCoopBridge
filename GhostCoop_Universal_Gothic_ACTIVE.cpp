#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <DynamicOutput/Output.hpp>
#include <Mod/CppUserModBase.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <Windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>
#include <utility>

namespace GhostCoopUniversal
{
    using namespace RC;
    using namespace Unreal;

    // ============================================================
    // Bridge files
    // ============================================================

    static constexpr const char* BRIDGE_DIR = "C:\\GhostCoopBridge";
    static constexpr const char* OUT_FILE = "C:\\GhostCoopBridge\\local_position.json";
    static constexpr const char* TMP_FILE = "C:\\GhostCoopBridge\\local_position.universal.tmp";
    static constexpr const char* DEBUG_FILE = "C:\\GhostCoopBridge\\native_debug.txt";
    static constexpr const char* RAYCAST_DEBUG_FILE = "C:\\GhostCoopBridge\\raycast_debug.txt";
    static constexpr const char* CANDIDATE_DEBUG_FILE = "C:\\GhostCoopBridge\\candidate_debug.txt";
    static constexpr const char* REMOTE_PLAYERS_FILE = "C:\\GhostCoopBridge\\remote_players.txt";
    static constexpr const char* BUILD_TAG = "GhostCoop_Universal_GOTHIC_ACTIVE_2026_07_08";

    // ============================================================
    // GAME PROFILE SWITCH
    // ============================================================
    // Toto je hlavný prepínač. Pri builde si tu nastavíš hru:
    //
    //     "gothic"  -> Gothic Remake offsets
    //     "sh2"     -> Silent Hill 2 offsets
    //     "auto"    -> fallback podľa názvu .exe, iba keď chceš autodetekciu
    //
    // Pre nový profil pridáš nový záznam do PROFILES[] nižšie a sem dáš jeho id.
    static constexpr const char* ACTIVE_GAME_PROFILE = "gothic";

    // false = keď ACTIVE_GAME_PROFILE nesedí/nefunguje, neskúša ostatné profily.
    // true  = po zlyhaní prepínača skúsi ostatné profily podľa exe/profiles listu.
    static constexpr bool ALLOW_PROFILE_AUTODETECT_WHEN_SWITCH_FAILS = false;

    static constexpr bool WRITE_DEBUG_FILE = true;
    static constexpr bool WRITE_RAYCAST_DEBUG_FILE = true;

    // Keep this conservative. The Python/overlay side can interpolate/smooth.
    static constexpr auto BOOT_DELAY = std::chrono::seconds(8);
    static constexpr auto SAMPLE_INTERVAL = std::chrono::milliseconds(50);
    static constexpr auto CONTROLLER_SEARCH_INTERVAL = std::chrono::milliseconds(500);
    static constexpr auto HEARTBEAT_INTERVAL = std::chrono::seconds(1);

    // Custom UE4SS for Lies of P is now loaded, so we do NOT need the old 4-minute
    // startup delay anymore. Keep discovery throttled so heavy offset probing does
    // not run every 50ms before we have a stable snapshot.
    static constexpr auto LIESOFP_DISCOVERY_INTERVAL = std::chrono::seconds(2);

    // Lies of P probe mode can accidentally accept a valid UObject chain that is
    // not the real player, because 0,0,0 is technically a sane FVector. Reject
    // origin-like probe hits so the scanner continues to the next offset combo.
    static constexpr bool LIESOFP_REJECT_ORIGIN_PROBE_SNAPSHOT = true;
    static constexpr double LIESOFP_ORIGIN_REJECT_RADIUS = 25.0;

    // Reject false positives like 0,0,85.686. That is not a real map position;
    // it is usually a capsule/root vertical value read from a structurally valid
    // but wrong UObject chain.
    static constexpr bool LIESOFP_REJECT_VERTICAL_AXIS_PROBE_SNAPSHOT = true;
    static constexpr double LIESOFP_VERTICAL_AXIS_REJECT_XY_RADIUS = 25.0;
    static constexpr double LIESOFP_MIN_DIRECT_ACTOR_ACCEPT_XY_RADIUS = 100.0;

    // Reject garbage reads such as 0, 60976220, 0. Those are structurally valid
    // floats from a UObject chain, but not a usable world position for this game.
    static constexpr bool LIESOFP_REJECT_IMPLAUSIBLE_PROBE_SNAPSHOT = true;
    static constexpr double LIESOFP_MAX_ABS_POSITION = 2000000.0;
    static constexpr double LIESOFP_MAX_ABS_Z = 250000.0;

    // Candidate debug is capped to avoid creating huge bridge files when the
    // probe scans many offset combinations.
    static constexpr int MAX_CANDIDATE_DEBUG_LINES = 5000;

    // ============================================================
    // Basic UE raw structs
    // ============================================================

    // UE5/LWC FVector/FRotator are double based. Both of your current working
    // files use double vectors, so this universal file does too.
    struct Vec3
    {
        double x;
        double y;
        double z;
    };

    struct Rot3
    {
        double pitch;
        double yaw;
        double roll;
    };

    struct Vec3F
    {
        float x;
        float y;
        float z;
    };

    struct Rot3F
    {
        float pitch;
        float yaw;
        float roll;
    };

    struct LinearColorRaw
    {
        float r;
        float g;
        float b;
        float a;
    };

    struct TArrayRaw
    {
        void* data;
        int32_t num;
        int32_t max;
    };

    static_assert(sizeof(Vec3) == 0x18, "Vec3 must be UE5 double FVector size");
    static_assert(sizeof(Rot3) == 0x18, "Rot3 must be UE5 double FRotator size");
    static_assert(sizeof(TArrayRaw) == 0x10, "TArray raw layout mismatch");

    // SH2 working layout for:
    // UKismetSystemLibrary::LineTraceSingle(WorldContext, Start, End, TraceChannel,
    // TraceComplex, ActorsToIgnore, DrawDebugType, OutHit, IgnoreSelf, Colors, DrawTime)
    static constexpr size_t UE5_FHITRESULT_SIZE = 0xE8;

    struct LineTraceSingleParamsD
    {
        UObject* WorldContextObject;          // 0x0000
        Vec3 Start;                           // 0x0008
        Vec3 End;                             // 0x0020
        uint8_t TraceChannel;                 // 0x0038
        bool bTraceComplex;                   // 0x0039
        uint8_t Pad_003A[0x06];               // 0x003A
        TArrayRaw ActorsToIgnore;             // 0x0040
        uint8_t DrawDebugType;                // 0x0050
        uint8_t Pad_0051[0x07];               // 0x0051
        uint8_t OutHit[UE5_FHITRESULT_SIZE];  // 0x0058
        bool bIgnoreSelf;                     // 0x0140
        uint8_t Pad_0141[0x03];               // 0x0141
        LinearColorRaw TraceColor;            // 0x0144
        LinearColorRaw TraceHitColor;         // 0x0154
        float DrawTime;                       // 0x0164
        bool ReturnValue;                     // 0x0168
    };

    static_assert(offsetof(LineTraceSingleParamsD, Start) == 0x08, "LineTrace params mismatch");
    static_assert(offsetof(LineTraceSingleParamsD, End) == 0x20, "LineTrace params mismatch");
    static_assert(offsetof(LineTraceSingleParamsD, ActorsToIgnore) == 0x40, "LineTrace params mismatch");
    static_assert(offsetof(LineTraceSingleParamsD, OutHit) == 0x58, "LineTrace params mismatch");
    static_assert(offsetof(LineTraceSingleParamsD, ReturnValue) == 0x168, "LineTrace params mismatch");

    // ============================================================
    // Game profiles
    // ============================================================

    enum class CameraMode
    {
        DirectCameraManager,
        CameraCachePOV
    };

    enum class VectorStorageMode
    {
        UE5Double,
        UE4Float
    };

    struct Offsets
    {
        // Controller -> pawn. Some games expose both AController::Pawn and
        // APlayerController::AcknowledgedPawn. We try acknowledged first if present.
        uintptr_t controllerPawn = 0;
        uintptr_t controllerAcknowledgedPawn = 0;
        uintptr_t controllerCameraManager = 0;

        uintptr_t pawnRootComponent = 0;
        uintptr_t sceneRelativeLocation = 0;
        uintptr_t sceneRelativeRotation = 0;

        CameraMode cameraMode = CameraMode::DirectCameraManager;

        // CameraMode::DirectCameraManager
        uintptr_t cameraManagerLocation = 0;
        uintptr_t cameraManagerRotation = 0;
        uintptr_t cameraManagerFOV = 0;

        // CameraMode::CameraCachePOV
        uintptr_t cameraCachePrivate = 0;
        uintptr_t cameraCachePOV = 0;
        uintptr_t povLocation = 0;
        uintptr_t povRotation = 0;
        uintptr_t povFOV = 0;
    };

    struct GameProfile
    {
        const char* id;
        const char* displayName;
        std::array<const char*, 4> exeHints;
        Offsets offsets;

        VectorStorageMode vectorStorageMode = VectorStorageMode::UE5Double;

        // false for already-known working profiles.
        // true for new UE4 profiles where we want to keep the universal code,
        // but allow a small safe offset probe instead of deleting Gothic/SH2 code.
        bool probeCommonOffsets = false;

        bool tryRaycast;

        // Gothic does not necessarily use the same TraceTypeQuery channel as SH2.
        // Instead of hardcoding only 2, Gothic tries multiple channels and treats
        // ANY hit on any tested channel as "blocked".
        std::array<uint8_t, 8> traceChannels;
        int traceChannelCount;

        int visibleRaysRequired;
        double maxVisibleDistance;
    };

    static constexpr GameProfile PROFILES[] = {
        {
            "gothic",
            "Gothic Remake",
            { "gothic", "alkimia", "kit", "" },
            Offsets{
            // Gothic working offsets from your short/native file.
            0x338,      // PlayerController_Pawn
            0x000,      // no AcknowledgedPawn offset used in your Gothic file
            0x348,      // PlayerController_CameraManager
            0x1A0,      // Pawn_RootComponent
            0x128,      // SceneComponent_RelLocation
            0x140,      // SceneComponent_RelRotation
            CameraMode::DirectCameraManager,
            0x13A0,     // CameraManager_CamLocation
            0x13B8,     // CameraManager_CamRotation
            0x13D0,     // CameraManager_FOV
            0, 0, 0, 0, 0
        },
        VectorStorageMode::UE5Double,
        false,
        true,
        { 0, 1, 2, 3, 4, 5, 6, 7 },
        8,
        2,
        200000.0
    },
    {
        "sh2",
        "Silent Hill 2",
        { "shproto", "silent", "hill", "sh2" },
        Offsets{
            // SH2 working offsets from your longer position + raycast file.
            0x02F0,     // Controller_Pawn fallback
            0x0358,     // PC_AcknowledgedPawn
            0x0368,     // PC_PlayerCameraManager
            0x01B0,     // Actor_RootComponent
            0x0128,     // Scene_RelativeLocation
            0x0140,     // Scene_RelativeRotation
            CameraMode::CameraCachePOV,
            0, 0, 0,
            0x2350,     // PCM_CameraCachePrivate
            0x0010,     // CameraCache_POV
            0x0000,     // POV_Location
            0x0018,     // POV_Rotation
            0x0030      // POV_FOV
        },
        VectorStorageMode::UE5Double,
        false,
        true,
        { 2, 0, 1, 3, 4, 5, 6, 7 },
        1,              // SH2: keep original working behavior: only channel 2
        2,
        200000.0
    },
    {
        "liesofp",
        "Lies of P",
        { "lop", "liesofp", "lies of p", "neowiz" },
        Offsets{
            // First-pass UE4.27-style offsets. If these miss, probeCommonOffsets
            // tries a small set of common UE4 variants without touching Gothic/SH2.
            0x02A8,     // Controller->Pawn fallback candidate
            0x02A0,     // PlayerController->AcknowledgedPawn candidate
            0x02C0,     // PlayerController->PlayerCameraManager candidate
            0x0130,     // Actor->RootComponent candidate
            0x011C,     // SceneComponent->RelativeLocation candidate, UE4 float FVector
            0x0128,     // SceneComponent->RelativeRotation candidate, UE4 float FRotator
            CameraMode::CameraCachePOV,
            0, 0, 0,
            0x1A50,     // APlayerCameraManager->CameraCachePrivate candidate
            0x0010,     // FCameraCacheEntry->POV candidate
            0x0000,     // FMinimalViewInfo->Location
            0x000C,     // FMinimalViewInfo->Rotation
            0x0018      // FMinimalViewInfo->FOV
        },
        VectorStorageMode::UE4Float,
        true,
        false, // position-discovery safe build: disable raycast until XYZ is stable
        { 2, 0, 1, 3, 4, 5, 6, 7 },
        8,
        2,
        200000.0
    }
    };

    // ============================================================
    // Runtime data
    // ============================================================

    struct Snapshot
    {
        Vec3 loc{};
        Rot3 rot{};
        Vec3 camLoc{};
        Rot3 camRot{};
        float fov = 90.0f;

        UObject* controller = nullptr;
        UObject* pawn = nullptr;
        UObject* root = nullptr;
        UObject* cameraManager = nullptr;
        uintptr_t povAddress = 0;
        const GameProfile* profile = nullptr;

        Offsets effectiveOffsets{};
        bool hasEffectiveOffsets = false;
        std::string offsetSource;

        // Lies of P note:
        // RootComponent->RelativeLocation can be 0,0,0 for some valid-looking chains.
        // When that happens, we also probe likely ComponentToWorld translation offsets
        // and record which one rescued the real world-space location.
        bool usedWorldLocationOffset = false;
        uintptr_t worldLocationOffset = 0;
    };

    struct RemotePlayer
    {
        std::string name;
        Vec3 loc{};
    };

    struct VisibilityValue
    {
        bool known = false;
        bool visible = false;
        int cleanRays = 0;
        int validRays = 0;
    };

    struct WorldContextCandidate
    {
        const char* label = "unknown";
        UObject* object = nullptr;
    };

    struct CachedTraceVariant
    {
        bool locked = false;
        UObject* worldContext = nullptr;
        std::string label;
        uint8_t channel = 2;
        size_t hitResultSize = 0xE8;
        bool chosenFromBlockingHit = false;
        int failedFastCalls = 0;
    };

    static CachedTraceVariant g_traceCache{};

    // Last discovery diagnostics written when no snapshot is accepted.
    static std::string g_lastDiscoveryDebug;

    // Candidate diagnostics: records the first accepted/rejected structurally-valid
    // candidates so we can see what the probe is actually finding instead of
    // guessing from only the final snapshot.
    static std::string g_candidateDebug;
    static int g_candidateDebugLineCount = 0;

    static constexpr int FAST_VISIBLE_RAYS_REQUIRED = 2;
    static constexpr double SELF_SKIP_DISTANCE = 100.0;

    // Trees/foliage can be on a different trace response than walls.
    // Keep the cached fast variant for stability, but when the cached ray is clear,
    // optionally probe the same working world context + hit layout through all common channels.
    static constexpr bool ENABLE_FOLIAGE_CHANNEL_RESCUE = true;

    // If controller->PlayerCameraManager is null in Gothic, scan PlayerCameraManager objects.
    // This fixes narrow occluders such as tree trunks, where player-head fallback camera
    // can miss even though the real screen camera is looking through the object.
    static constexpr bool ENABLE_CAMERA_MANAGER_SCAN = true;

    // ============================================================
    // Helpers
    // ============================================================

    static std::string to_lower(std::string s)
    {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
            });
        return s;
    }

    static std::string trim(std::string s)
    {
        while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ' || s.back() == '\t'))
            s.pop_back();

        size_t first = 0;
        while (first < s.size() && (s[first] == ' ' || s[first] == '\t'))
            ++first;

        if (first > 0)
            s.erase(0, first);

        return s;
    }

    static std::string exe_name_lower()
    {
        char path[MAX_PATH]{};
        GetModuleFileNameA(nullptr, path, MAX_PATH);

        std::string p = path;
        const size_t slash = p.find_last_of("\\/");
        if (slash != std::string::npos)
            p = p.substr(slash + 1);

        return to_lower(p);
    }

    static const GameProfile* find_profile_by_id(const std::string& idText)
    {
        const std::string wanted = to_lower(trim(idText));

        if (wanted.empty())
            return nullptr;

        for (const auto& p : PROFILES)
        {
            const std::string id = to_lower(p.id ? p.id : "");
            if (id == wanted)
                return &p;
        }

        return nullptr;
    }

    static bool profile_matches_text(const GameProfile& p, const std::string& text)
    {
        if (text.empty())
            return false;

        const std::string id = to_lower(p.id ? p.id : "");
        const std::string display = to_lower(p.displayName ? p.displayName : "");

        if (!id.empty() && text.find(id) != std::string::npos)
            return true;

        if (!display.empty() && text.find(display) != std::string::npos)
            return true;

        for (const char* hint : p.exeHints)
        {
            if (!hint || !*hint)
                continue;

            const std::string h = to_lower(hint);
            if (text.find(h) != std::string::npos)
                return true;
        }

        return false;
    }

    static const GameProfile* find_profile_by_text(const std::string& text)
    {
        for (const auto& p : PROFILES)
        {
            if (profile_matches_text(p, text))
                return &p;
        }

        return nullptr;
    }

    static bool is_readable_protection(DWORD protect)
    {
        if (protect & PAGE_GUARD)
            return false;

        if (protect & PAGE_NOACCESS)
            return false;

        protect &= 0xff;

        switch (protect)
        {
        case PAGE_READONLY:
        case PAGE_READWRITE:
        case PAGE_WRITECOPY:
        case PAGE_EXECUTE_READ:
        case PAGE_EXECUTE_READWRITE:
        case PAGE_EXECUTE_WRITECOPY:
            return true;

        default:
            return false;
        }
    }

    static bool is_readable_range(const void* address, size_t size)
    {
        if (!address || size == 0)
            return false;

        const uintptr_t start = reinterpret_cast<uintptr_t>(address);
        const uintptr_t end = start + size;

        if (end < start)
            return false;

        uintptr_t current = start;

        while (current < end)
        {
            MEMORY_BASIC_INFORMATION mbi{};

            if (VirtualQuery(reinterpret_cast<const void*>(current), &mbi, sizeof(mbi)) == 0)
                return false;

            if (mbi.State != MEM_COMMIT)
                return false;

            if (!is_readable_protection(mbi.Protect))
                return false;

            const uintptr_t regionEnd = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
            if (regionEnd <= current)
                return false;

            current = std::min(regionEnd, end);
        }

        return true;
    }

    template <typename T>
    static bool safe_read_addr(uintptr_t address, T& out)
    {
        if (!address)
            return false;

        const void* ptr = reinterpret_cast<const void*>(address);

        if (!is_readable_range(ptr, sizeof(T)))
            return false;

        __try
        {
            std::memcpy(&out, ptr, sizeof(T));
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    template <typename T>
    static bool safe_read_offset(void* base, uintptr_t offset, T& out)
    {
        if (!base)
            return false;

        const uintptr_t b = reinterpret_cast<uintptr_t>(base);
        const uintptr_t addr = b + offset;

        if (addr < b)
            return false;

        return safe_read_addr<T>(addr, out);
    }

    static bool sane_number(double v)
    {
        return std::isfinite(v) && v > -100000000.0 && v < 100000000.0;
    }

    static bool sane_vec(const Vec3& v)
    {
        return sane_number(v.x) && sane_number(v.y) && sane_number(v.z);
    }

    static bool sane_rot(const Rot3& r)
    {
        return std::isfinite(r.pitch)
            && std::isfinite(r.yaw)
            && std::isfinite(r.roll)
            && r.pitch > -100000.0 && r.pitch < 100000.0
            && r.yaw > -100000.0 && r.yaw < 100000.0
            && r.roll > -100000.0 && r.roll < 100000.0;
    }

    static bool sane_fov(float fov)
    {
        return std::isfinite(fov) && fov >= 1.0f && fov <= 170.0f;
    }

    static double absd(double v)
    {
        return v < 0.0 ? -v : v;
    }

    static double distance_3d(const Vec3& a, const Vec3& b)
    {
        const double dx = a.x - b.x;
        const double dy = a.y - b.y;
        const double dz = a.z - b.z;
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    }

    static bool origin_like_vec(const Vec3& v, double radius = LIESOFP_ORIGIN_REJECT_RADIUS)
    {
        return distance_3d(v, Vec3{ 0.0, 0.0, 0.0 }) <= radius;
    }

    static bool vertical_axis_like_vec(const Vec3& v)
    {
        // Reject values that only have height but no map-space X/Y.
        // Example from the last run: 0,0,85.686012.
        if (!sane_vec(v))
            return false;

        const double xy = std::sqrt(v.x * v.x + v.y * v.y);
        return xy <= LIESOFP_VERTICAL_AXIS_REJECT_XY_RADIUS
            && absd(v.z) > LIESOFP_ORIGIN_REJECT_RADIUS;
    }

    static bool implausible_liesofp_probe_vec(const Vec3& v)
    {
        if (!sane_vec(v))
            return true;

        const double maxAbsXY = std::max(absd(v.x), absd(v.y));
        const double xy = std::sqrt(v.x * v.x + v.y * v.y);

        // Example from the last run: y=60,976,220. This is almost certainly
        // a misread transform/float field, not a player coordinate.
        if (maxAbsXY > LIESOFP_MAX_ABS_POSITION)
            return true;

        if (xy > LIESOFP_MAX_ABS_POSITION)
            return true;

        if (absd(v.z) > LIESOFP_MAX_ABS_Z)
            return true;

        return false;
    }

    static bool materially_changed(const Snapshot& a, const Snapshot& b)
    {
        if (absd(a.loc.x - b.loc.x) > 0.5) return true;
        if (absd(a.loc.y - b.loc.y) > 0.5) return true;
        if (absd(a.loc.z - b.loc.z) > 0.5) return true;

        if (absd(a.rot.pitch - b.rot.pitch) > 0.05) return true;
        if (absd(a.rot.yaw - b.rot.yaw) > 0.05) return true;
        if (absd(a.rot.roll - b.rot.roll) > 0.05) return true;

        if (absd(a.camLoc.x - b.camLoc.x) > 0.5) return true;
        if (absd(a.camLoc.y - b.camLoc.y) > 0.5) return true;
        if (absd(a.camLoc.z - b.camLoc.z) > 0.5) return true;

        if (absd(a.camRot.pitch - b.camRot.pitch) > 0.05) return true;
        if (absd(a.camRot.yaw - b.camRot.yaw) > 0.05) return true;
        if (absd(a.camRot.roll - b.camRot.roll) > 0.05) return true;

        if (absd(static_cast<double>(a.fov - b.fov)) > 0.05)
            return true;

        if (a.profile != b.profile)
            return true;

        return false;
    }

    static std::string json_escape(const std::string& value)
    {
        std::ostringstream ss;

        for (char c : value)
        {
            switch (c)
            {
            case '\\': ss << "\\\\"; break;
            case '"': ss << "\\\""; break;
            case '\n': ss << "\\n"; break;
            case '\r': ss << "\\r"; break;
            case '\t': ss << "\\t"; break;
            default: ss << c; break;
            }
        }

        return ss.str();
    }

    static void reset_candidate_debug(const char* reason)
    {
        g_candidateDebugLineCount = 0;

        std::ostringstream ss;
        ss << "build=" << BUILD_TAG << "\n";
        ss << "reason=" << (reason ? reason : "unknown") << "\n";
        ss << "note=This file records structurally valid probe candidates. Rejected candidates are useful because they show which offsets are almost-but-not-player, vertical-axis-only, or implausible huge garbage reads.\n\n";
        g_candidateDebug = ss.str();
    }

    static void append_candidate_debug_line(
        const char* sourceKind,
        const char* offsetSource,
        UObject* controller,
        UObject* pawn,
        UObject* root,
        const Offsets& offsets,
        const Vec3& loc,
        const Rot3& rot,
        bool usedWorldLocationOffset,
        uintptr_t worldLocationOffset,
        const char* decision)
    {
        if (g_candidateDebugLineCount >= MAX_CANDIDATE_DEBUG_LINES)
            return;

        std::ostringstream ss;
        ss << std::fixed << std::setprecision(6);
        ss << "candidate#" << g_candidateDebugLineCount << "\n";
        ss << "sourceKind=" << (sourceKind ? sourceKind : "unknown") << "\n";
        ss << "offsetSource=" << (offsetSource ? offsetSource : "unknown") << "\n";
        ss << "decision=" << (decision ? decision : "unknown") << "\n";

        ss << std::hex;
        ss << "controller=0x" << reinterpret_cast<uintptr_t>(controller) << "\n";
        ss << "pawn=0x" << reinterpret_cast<uintptr_t>(pawn) << "\n";
        ss << "root=0x" << reinterpret_cast<uintptr_t>(root) << "\n";
        ss << "controllerPawn=0x" << offsets.controllerPawn << "\n";
        ss << "controllerAcknowledgedPawn=0x" << offsets.controllerAcknowledgedPawn << "\n";
        ss << "controllerCameraManager=0x" << offsets.controllerCameraManager << "\n";
        ss << "pawnRootComponent=0x" << offsets.pawnRootComponent << "\n";
        ss << "sceneRelativeLocation=0x" << offsets.sceneRelativeLocation << "\n";
        ss << "sceneRelativeRotation=0x" << offsets.sceneRelativeRotation << "\n";
        ss << "usedWorldLocationOffset=" << (usedWorldLocationOffset ? "true" : "false") << "\n";
        ss << "worldLocationOffset=0x" << worldLocationOffset << "\n";
        ss << "cameraCachePrivate=0x" << offsets.cameraCachePrivate << "\n";
        ss << std::dec;

        const double xy = std::sqrt(loc.x * loc.x + loc.y * loc.y);
        ss << "loc=" << loc.x << ", " << loc.y << ", " << loc.z << "\n";
        ss << "rot=" << rot.pitch << ", " << rot.yaw << ", " << rot.roll << "\n";
        ss << "xyRadius=" << xy << "\n";
        ss << "originLike=" << (origin_like_vec(loc) ? "true" : "false") << "\n";
        ss << "verticalAxisLike=" << (vertical_axis_like_vec(loc) ? "true" : "false") << "\n";
        ss << "implausibleLike=" << (implausible_liesofp_probe_vec(loc) ? "true" : "false") << "\n";
        ss << "----\n";

        g_candidateDebug += ss.str();
        ++g_candidateDebugLineCount;
    }

    static bool write_text_atomic(const char* finalPath, const std::string& text)
    {
        CreateDirectoryA(BRIDGE_DIR, nullptr);

        const std::string tmpPath = std::string(finalPath) + ".tmp";

        HANDLE file = CreateFileA(
            tmpPath.c_str(),
            GENERIC_WRITE,
            FILE_SHARE_READ,
            nullptr,
            CREATE_ALWAYS,
            FILE_ATTRIBUTE_TEMPORARY | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED,
            nullptr
        );

        if (file == INVALID_HANDLE_VALUE)
            return false;

        DWORD written = 0;
        const BOOL writeOk = WriteFile(
            file,
            text.data(),
            static_cast<DWORD>(text.size()),
            &written,
            nullptr
        );

        FlushFileBuffers(file);
        CloseHandle(file);

        if (!writeOk || written != static_cast<DWORD>(text.size()))
        {
            DeleteFileA(tmpPath.c_str());
            return false;
        }

        for (int attempt = 0; attempt < 8; ++attempt)
        {
            if (MoveFileExA(
                tmpPath.c_str(),
                finalPath,
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            {
                return true;
            }

            Sleep(2);
        }

        DeleteFileA(tmpPath.c_str());
        return false;
    }

    static std::vector<std::string> split_line(const std::string& s, char sep)
    {
        std::vector<std::string> out;
        std::stringstream ss(s);
        std::string item;

        while (std::getline(ss, item, sep))
            out.push_back(item);

        return out;
    }

    static std::vector<RemotePlayer> read_remote_players()
    {
        std::vector<RemotePlayer> players;

        std::ifstream file(REMOTE_PLAYERS_FILE);
        if (!file.is_open())
            return players;

        std::string line;
        while (std::getline(file, line))
        {
            line = trim(line);
            if (line.empty())
                continue;

            const auto p = split_line(line, '|');
            if (p.size() < 4)
                continue;

            RemotePlayer rp{};
            rp.name = p[0];

            try
            {
                rp.loc.x = std::stod(p[1]);
                rp.loc.y = std::stod(p[2]);
                rp.loc.z = std::stod(p[3]);
            }
            catch (...)
            {
                continue;
            }

            if (sane_vec(rp.loc))
                players.push_back(rp);
        }

        return players;
    }

    // ============================================================
    // Snapshot reader
    // ============================================================

    static Vec3 to_vec3(const Vec3F& v)
    {
        return Vec3{ static_cast<double>(v.x), static_cast<double>(v.y), static_cast<double>(v.z) };
    }

    static Rot3 to_rot3(const Rot3F& r)
    {
        return Rot3{ static_cast<double>(r.pitch), static_cast<double>(r.yaw), static_cast<double>(r.roll) };
    }

    static bool read_scene_vec(const GameProfile& profile, UObject* object, uintptr_t offset, Vec3& out)
    {
        if (profile.vectorStorageMode == VectorStorageMode::UE4Float)
        {
            Vec3F tmp{};
            if (!safe_read_offset(object, offset, tmp))
                return false;

            out = to_vec3(tmp);
            return sane_vec(out);
        }

        if (!safe_read_offset(object, offset, out))
            return false;

        return sane_vec(out);
    }

    static bool read_scene_rot(const GameProfile& profile, UObject* object, uintptr_t offset, Rot3& out)
    {
        if (profile.vectorStorageMode == VectorStorageMode::UE4Float)
        {
            Rot3F tmp{};
            if (!safe_read_offset(object, offset, tmp))
                return false;

            out = to_rot3(tmp);
            return sane_rot(out);
        }

        if (!safe_read_offset(object, offset, out))
            return false;

        return sane_rot(out);
    }


    static bool try_read_component_world_location(
        const GameProfile& profile,
        UObject* sceneComponent,
        Vec3& out,
        uintptr_t& outOffset)
    {
        if (!sceneComponent)
            return false;

        // UE4 USceneComponent::ComponentToWorld is an FTransform.
        // FTransform translation is commonly at +0x10 inside the transform.
        // Different custom engine layouts shift ComponentToWorld, so we probe
        // likely *translation* addresses directly as Vec3F.
        static constexpr std::array<uintptr_t, 18> WORLD_LOCATION_OFFSETS = {
            0x0190, 0x0198, 0x01A0, 0x01A8,
            0x01B0, 0x01B8, 0x01C0, 0x01C8,
            0x01D0, 0x01D8, 0x01E0, 0x01E8,
            0x01F0, 0x01F8, 0x0200, 0x0208,
            0x0210, 0x0220
        };

        for (uintptr_t off : WORLD_LOCATION_OFFSETS)
        {
            Vec3 candidate{};
            if (!read_scene_vec(profile, sceneComponent, off, candidate))
                continue;

            if (!sane_vec(candidate))
                continue;

            if (origin_like_vec(candidate))
                continue;

            if (vertical_axis_like_vec(candidate))
                continue;

            if (implausible_liesofp_probe_vec(candidate))
                continue;

            out = candidate;
            outOffset = off;
            return true;
        }

        return false;
    }

    static double camera_score_from_player(const Vec3& playerLoc, const Vec3& camLoc)
    {
        const double d = distance_3d(playerLoc, camLoc);

        // Real third-person camera should usually be near the pawn, not thousands away,
        // and not exactly equal to the player fallback.
        if (d < 20.0)
            return 999999999.0;

        if (d > 3500.0)
            return 999999999.0;

        return d;
    }

    static bool try_read_camera_manager_direct_with_offsets(
        const GameProfile& profile,
        const Offsets& offsets,
        UObject* cameraManager,
        Snapshot& out)
    {
        if (!cameraManager)
            return false;

        if (offsets.cameraMode != CameraMode::DirectCameraManager)
            return false;

        Vec3 camLoc{};
        Rot3 camRot{};
        float fov = 90.0f;

        const bool ok =
            read_scene_vec(profile, cameraManager, offsets.cameraManagerLocation, camLoc)
            && read_scene_rot(profile, cameraManager, offsets.cameraManagerRotation, camRot)
            && safe_read_offset(cameraManager, offsets.cameraManagerFOV, fov)
            && sane_vec(camLoc)
            && sane_rot(camRot)
            && sane_fov(fov);

        if (!ok)
            return false;

        out.camLoc = camLoc;
        out.camRot = camRot;
        out.fov = fov;
        out.cameraManager = cameraManager;
        return true;
    }

    static bool try_read_camera_cache_with_offsets(
        const GameProfile& profile,
        const Offsets& offsets,
        UObject* cameraManager,
        Snapshot& out)
    {
        if (!cameraManager)
            return false;

        if (offsets.cameraMode != CameraMode::CameraCachePOV)
            return false;

        const uintptr_t camAddr = reinterpret_cast<uintptr_t>(cameraManager);
        const uintptr_t pov = camAddr + offsets.cameraCachePrivate + offsets.cameraCachePOV;
        out.povAddress = pov;

        Vec3 camLoc{};
        Rot3 camRot{};
        float fov = 90.0f;

        bool ok = false;

        if (profile.vectorStorageMode == VectorStorageMode::UE4Float)
        {
            Vec3F camLocF{};
            Rot3F camRotF{};

            ok =
                safe_read_addr(pov + offsets.povLocation, camLocF)
                && safe_read_addr(pov + offsets.povRotation, camRotF)
                && safe_read_addr(pov + offsets.povFOV, fov);

            if (ok)
            {
                camLoc = to_vec3(camLocF);
                camRot = to_rot3(camRotF);
            }
        }
        else
        {
            ok =
                safe_read_addr(pov + offsets.povLocation, camLoc)
                && safe_read_addr(pov + offsets.povRotation, camRot)
                && safe_read_addr(pov + offsets.povFOV, fov);
        }

        ok = ok && sane_vec(camLoc) && sane_rot(camRot) && sane_fov(fov);

        if (!ok)
            return false;

        out.camLoc = camLoc;
        out.camRot = camRot;
        out.fov = fov;
        out.cameraManager = cameraManager;
        return true;
    }

    static bool try_read_camera_manager_direct(const GameProfile& profile, UObject* cameraManager, Snapshot& out)
    {
        const Offsets& offsets = out.hasEffectiveOffsets ? out.effectiveOffsets : profile.offsets;
        return try_read_camera_manager_direct_with_offsets(profile, offsets, cameraManager, out);
    }

    static bool scan_player_camera_manager_with_offsets(const GameProfile& profile, const Offsets& offsets, Snapshot& out)
    {
        if (!ENABLE_CAMERA_MANAGER_SCAN)
            return false;

        std::vector<UObject*> cameraManagers;
        UObjectGlobals::FindAllOf(STR("PlayerCameraManager"), cameraManagers);

        bool found = false;
        Snapshot best = out;
        double bestScore = 999999999.0;

        for (UObject* cam : cameraManagers)
        {
            if (!cam)
                continue;

            Snapshot candidate = out;

            bool ok = false;
            if (offsets.cameraMode == CameraMode::DirectCameraManager)
                ok = try_read_camera_manager_direct_with_offsets(profile, offsets, cam, candidate);
            else
                ok = try_read_camera_cache_with_offsets(profile, offsets, cam, candidate);

            if (!ok)
                continue;

            const double score = camera_score_from_player(out.loc, candidate.camLoc);
            if (score < bestScore)
            {
                bestScore = score;
                best = candidate;
                found = true;
            }
        }

        if (!found)
            return false;

        out = best;
        return true;
    }

    static bool scan_player_camera_manager(const GameProfile& profile, Snapshot& out)
    {
        const Offsets& offsets = out.hasEffectiveOffsets ? out.effectiveOffsets : profile.offsets;
        return scan_player_camera_manager_with_offsets(profile, offsets, out);
    }

    static bool read_camera_with_offsets(
        const GameProfile& profile,
        const Offsets& offsets,
        UObject* controller,
        UObject* cameraManager,
        Snapshot& out)
    {
        // Safe fallback: even if camera offsets are wrong, we still write player XYZ.
        out.camLoc = Vec3{ out.loc.x, out.loc.y, out.loc.z + 180.0 };
        out.camRot = out.rot;
        out.fov = 90.0f;
        out.cameraManager = cameraManager;
        out.povAddress = 0;

        if (controller && cameraManager)
        {
            bool ok = false;

            if (offsets.cameraMode == CameraMode::DirectCameraManager)
                ok = try_read_camera_manager_direct_with_offsets(profile, offsets, cameraManager, out);
            else
                ok = try_read_camera_cache_with_offsets(profile, offsets, cameraManager, out);

            if (ok)
                return true;
        }

        // Gothic sometimes had controller->cameraManager == nullptr in your debug.
        // For new probe profiles such as Lies of P, avoid global PlayerCameraManager
        // FindAllOf on every update. The crash dump showed UE4SS AV while our mod was
        // on the stack, so this stability build keeps camera fallback only until XYZ
        // is confirmed stable.
        if (!profile.probeCommonOffsets)
            scan_player_camera_manager_with_offsets(profile, offsets, out);

        return true;
    }

    static bool read_camera(const GameProfile& profile, UObject* controller, UObject* cameraManager, Snapshot& out)
    {
        const Offsets& offsets = out.hasEffectiveOffsets ? out.effectiveOffsets : profile.offsets;
        return read_camera_with_offsets(profile, offsets, controller, cameraManager, out);
    }

    static bool safe_snapshot_with_offsets(
        const GameProfile& profile,
        const Offsets& offsets,
        UObject* controller,
        Snapshot& out,
        const char* offsetSource)
    {
        if (!controller)
            return false;

        UObject* pawn = nullptr;

        if (offsets.controllerAcknowledgedPawn != 0)
            safe_read_offset(controller, offsets.controllerAcknowledgedPawn, pawn);

        if (!pawn && offsets.controllerPawn != 0)
            safe_read_offset(controller, offsets.controllerPawn, pawn);

        if (!pawn)
            return false;

        UObject* root = nullptr;
        if (!safe_read_offset(pawn, offsets.pawnRootComponent, root) || !root)
            return false;

        Vec3 loc{};
        Rot3 rot{};

        if (!read_scene_vec(profile, root, offsets.sceneRelativeLocation, loc))
            return false;

        if (!read_scene_rot(profile, root, offsets.sceneRelativeRotation, rot))
            return false;

        if (!sane_vec(loc) || !sane_rot(rot))
            return false;

        bool usedWorldLocationOffset = false;
        uintptr_t worldLocationOffset = 0;

        // Lies of P: RelativeLocation can be origin even when the component has
        // a valid world-space transform. Before rejecting 0,0,0, try reading
        // likely ComponentToWorld translation offsets from the same root.
        if (profile.probeCommonOffsets && origin_like_vec(loc))
        {
            Vec3 worldLoc{};
            uintptr_t worldLocOffset = 0;
            if (try_read_component_world_location(profile, root, worldLoc, worldLocOffset))
            {
                loc = worldLoc;
                usedWorldLocationOffset = true;
                worldLocationOffset = worldLocOffset;
            }
        }

        // Probe-only guard for Lies of P/new UE4 profiles:
        // a UObject chain at the world origin can look structurally valid but is not
        // the actual player. Reject origin-like LOCATION regardless of rotation
        // and continue scanning later candidates.
        if (profile.probeCommonOffsets
            && LIESOFP_REJECT_ORIGIN_PROBE_SNAPSHOT
            && origin_like_vec(loc))
        {
            append_candidate_debug_line(
                "controller_chain",
                offsetSource,
                controller,
                pawn,
                root,
                offsets,
                loc,
                rot,
                usedWorldLocationOffset,
                worldLocationOffset,
                "reject_origin"
            );
            return false;
        }

        // The new false positive was 0,0,85.686: not origin, but still
        // glued to the vertical world axis. Reject that too in probe mode.
        if (profile.probeCommonOffsets
            && LIESOFP_REJECT_VERTICAL_AXIS_PROBE_SNAPSHOT
            && vertical_axis_like_vec(loc))
        {
            append_candidate_debug_line(
                "controller_chain",
                offsetSource,
                controller,
                pawn,
                root,
                offsets,
                loc,
                rot,
                usedWorldLocationOffset,
                worldLocationOffset,
                "reject_vertical_axis"
            );
            return false;
        }

        // Reject gigantic/garbage coordinates from misread transform fields.
        if (profile.probeCommonOffsets
            && LIESOFP_REJECT_IMPLAUSIBLE_PROBE_SNAPSHOT
            && implausible_liesofp_probe_vec(loc))
        {
            append_candidate_debug_line(
                "controller_chain",
                offsetSource,
                controller,
                pawn,
                root,
                offsets,
                loc,
                rot,
                usedWorldLocationOffset,
                worldLocationOffset,
                "reject_implausible_position"
            );
            return false;
        }

        append_candidate_debug_line(
            "controller_chain",
            offsetSource,
            controller,
            pawn,
            root,
            offsets,
            loc,
            rot,
            usedWorldLocationOffset,
            worldLocationOffset,
            "accept"
        );

        UObject* cam = nullptr;
        if (offsets.controllerCameraManager != 0)
            safe_read_offset(controller, offsets.controllerCameraManager, cam);

        out = Snapshot{};
        out.loc = loc;
        out.rot = rot;
        out.controller = controller;
        out.pawn = pawn;
        out.root = root;
        out.profile = &profile;
        out.effectiveOffsets = offsets;
        out.hasEffectiveOffsets = true;
        out.offsetSource = offsetSource ? offsetSource : "profile";
        out.usedWorldLocationOffset = usedWorldLocationOffset;
        out.worldLocationOffset = worldLocationOffset;

        read_camera_with_offsets(profile, offsets, controller, cam, out);

        return true;
    }

    static std::vector<Offsets> build_common_ue4_offset_candidates(const GameProfile& profile)
    {
        std::vector<Offsets> candidates;
        candidates.push_back(profile.offsets);

        if (!profile.probeCommonOffsets)
            return candidates;

        // Common UE4/UE4.27 candidate ranges. This is intentionally small:
        // enough to bootstrap a new profile, not a huge per-frame scanner.
        static constexpr std::array<uintptr_t, 16> PAWN_OFFSETS = {
            0x0280, 0x0288, 0x0290, 0x0298,
            0x02A0, 0x02A8, 0x02B0, 0x02B8,
            0x02C0, 0x02D0, 0x02E0, 0x02F0,
            0x0300, 0x0338, 0x0358, 0x0368
        };

        static constexpr std::array<uintptr_t, 7> CAMERA_MANAGER_OFFSETS = {
            0x02B8, 0x02C0, 0x02C8, 0x02D0, 0x02E0, 0x0348, 0x0368
        };

        static constexpr std::array<uintptr_t, 8> ROOT_OFFSETS = {
            0x0130, 0x0138, 0x0140, 0x0148,
            0x0150, 0x01A0, 0x01A8, 0x01B0
        };

        struct LocRotPair
        {
            uintptr_t loc;
            uintptr_t rot;
        };

        static constexpr std::array<LocRotPair, 13> LOC_ROT_OFFSETS = {
            LocRotPair{ 0x0100, 0x010C },
            LocRotPair{ 0x0108, 0x0114 },
            LocRotPair{ 0x0110, 0x011C },
            LocRotPair{ 0x0118, 0x0124 },
            LocRotPair{ 0x011C, 0x0128 },
            LocRotPair{ 0x0120, 0x012C },
            LocRotPair{ 0x0124, 0x0130 },
            LocRotPair{ 0x0128, 0x0134 },
            LocRotPair{ 0x0130, 0x013C },
            LocRotPair{ 0x0134, 0x0140 },
            LocRotPair{ 0x0140, 0x014C },
            LocRotPair{ 0x0150, 0x015C },
            LocRotPair{ 0x0160, 0x016C }
        };

        static constexpr std::array<uintptr_t, 8> CAMERA_CACHE_OFFSETS = {
            0x1A00, 0x1A10, 0x1A50, 0x1A60, 0x1A70, 0x1AB0, 0x1B00, 0x2350
        };

        for (uintptr_t pawnOffset : PAWN_OFFSETS)
        {
            for (uintptr_t rootOffset : ROOT_OFFSETS)
            {
                for (LocRotPair lr : LOC_ROT_OFFSETS)
                {
                    for (uintptr_t camManagerOffset : CAMERA_MANAGER_OFFSETS)
                    {
                        // AcknowledgedPawn and Pawn are both tried by safe_snapshot_with_offsets.
                        // Put the same candidate into both slots for bootstrap purposes.
                        Offsets o = profile.offsets;
                        o.controllerPawn = pawnOffset;
                        o.controllerAcknowledgedPawn = pawnOffset;
                        o.controllerCameraManager = camManagerOffset;
                        o.pawnRootComponent = rootOffset;
                        o.sceneRelativeLocation = lr.loc;
                        o.sceneRelativeRotation = lr.rot;

                        // Camera cache candidates are tried in a second, limited loop.
                        // Use the profile default here first.
                        candidates.push_back(o);
                    }
                }
            }
        }

        // Add a few camera cache variants for the most likely pawn/root/scene offsets.
        // This keeps position discovery quick and gives camera a chance once XYZ works.
        const size_t baseCount = candidates.size();
        const size_t maxBase = std::min<size_t>(baseCount, 64);
        for (size_t i = 0; i < maxBase; ++i)
        {
            for (uintptr_t camCache : CAMERA_CACHE_OFFSETS)
            {
                Offsets o = candidates[i];
                o.cameraMode = CameraMode::CameraCachePOV;
                o.cameraCachePrivate = camCache;
                o.cameraCachePOV = 0x0010;
                o.povLocation = 0x0000;
                o.povRotation = 0x000C;
                o.povFOV = 0x0018;
                candidates.push_back(o);
            }
        }

        return candidates;
    }

    static bool safe_snapshot_with_profile(const GameProfile& profile, UObject* controller, Snapshot& out)
    {
        if (!controller)
            return false;

        // Existing known profiles are still just one direct read.
        if (!profile.probeCommonOffsets)
            return safe_snapshot_with_offsets(profile, profile.offsets, controller, out, "profile");

        // New profiles such as Lies of P can probe common UE4 offsets while
        // preserving the Gothic/SH2 paths untouched.
        const auto candidates = build_common_ue4_offset_candidates(profile);

        for (size_t i = 0; i < candidates.size(); ++i)
        {
            std::string source = "probe#" + std::to_string(i);
            if (safe_snapshot_with_offsets(profile, candidates[i], controller, out, source.c_str()))
                return true;
        }

        return false;
    }


    struct DirectActorScanStats
    {
        int characterCount = 0;
        int pawnCount = 0;
        int testedObjects = 0;
        int rootFails = 0;
        int transformFails = 0;
        int originRejects = 0;
        int accepted = 0;
    };

    static std::vector<Offsets> build_direct_actor_offset_candidates(const GameProfile& profile)
    {
        std::vector<Offsets> out;

        static constexpr std::array<uintptr_t, 10> ROOT_OFFSETS = {
            0x0120, 0x0128, 0x0130, 0x0138, 0x0140,
            0x0148, 0x0150, 0x01A0, 0x01A8, 0x01B0
        };

        struct LocRotPair
        {
            uintptr_t loc;
            uintptr_t rot;
        };

        static constexpr std::array<LocRotPair, 16> LOC_ROT_OFFSETS = {
            LocRotPair{ 0x0100, 0x010C },
            LocRotPair{ 0x0108, 0x0114 },
            LocRotPair{ 0x0110, 0x011C },
            LocRotPair{ 0x0118, 0x0124 },
            LocRotPair{ 0x011C, 0x0128 },
            LocRotPair{ 0x0120, 0x012C },
            LocRotPair{ 0x0124, 0x0130 },
            LocRotPair{ 0x0128, 0x0134 },
            LocRotPair{ 0x0130, 0x013C },
            LocRotPair{ 0x0134, 0x0140 },
            LocRotPair{ 0x0140, 0x014C },
            LocRotPair{ 0x0148, 0x0154 },
            LocRotPair{ 0x0150, 0x015C },
            LocRotPair{ 0x0158, 0x0164 },
            LocRotPair{ 0x0160, 0x016C },
            LocRotPair{ 0x0168, 0x0174 }
        };

        for (uintptr_t rootOffset : ROOT_OFFSETS)
        {
            for (LocRotPair lr : LOC_ROT_OFFSETS)
            {
                Offsets o = profile.offsets;
                o.pawnRootComponent = rootOffset;
                o.sceneRelativeLocation = lr.loc;
                o.sceneRelativeRotation = lr.rot;
                out.push_back(o);
            }
        }

        return out;
    }

    static bool safe_snapshot_from_direct_pawn_like_object(
        const GameProfile& profile,
        const Offsets& offsets,
        UObject* pawnLike,
        Snapshot& out,
        const char* offsetSource,
        DirectActorScanStats& stats)
    {
        if (!pawnLike)
            return false;

        UObject* root = nullptr;
        if (!safe_read_offset(pawnLike, offsets.pawnRootComponent, root) || !root)
        {
            ++stats.rootFails;
            return false;
        }

        Vec3 loc{};
        Rot3 rot{};

        const bool locOk = read_scene_vec(profile, root, offsets.sceneRelativeLocation, loc);
        const bool rotOk = read_scene_rot(profile, root, offsets.sceneRelativeRotation, rot);

        if (!locOk || !rotOk || !sane_vec(loc) || !sane_rot(rot))
        {
            ++stats.transformFails;
            return false;
        }

        bool usedWorldLocationOffset = false;
        uintptr_t worldLocationOffset = 0;

        if (origin_like_vec(loc))
        {
            Vec3 worldLoc{};
            uintptr_t worldLocOffset = 0;
            if (try_read_component_world_location(profile, root, worldLoc, worldLocOffset))
            {
                loc = worldLoc;
                usedWorldLocationOffset = true;
                worldLocationOffset = worldLocOffset;
            }
        }

        if (profile.probeCommonOffsets && origin_like_vec(loc))
        {
            ++stats.originRejects;
            append_candidate_debug_line(
                "direct_actor",
                offsetSource,
                nullptr,
                pawnLike,
                root,
                offsets,
                loc,
                rot,
                usedWorldLocationOffset,
                worldLocationOffset,
                "reject_origin"
            );
            return false;
        }

        if (profile.probeCommonOffsets
            && LIESOFP_REJECT_VERTICAL_AXIS_PROBE_SNAPSHOT
            && vertical_axis_like_vec(loc))
        {
            ++stats.originRejects;
            append_candidate_debug_line(
                "direct_actor",
                offsetSource,
                nullptr,
                pawnLike,
                root,
                offsets,
                loc,
                rot,
                usedWorldLocationOffset,
                worldLocationOffset,
                "reject_vertical_axis"
            );
            return false;
        }

        if (profile.probeCommonOffsets
            && LIESOFP_REJECT_IMPLAUSIBLE_PROBE_SNAPSHOT
            && implausible_liesofp_probe_vec(loc))
        {
            ++stats.transformFails;
            append_candidate_debug_line(
                "direct_actor",
                offsetSource,
                nullptr,
                pawnLike,
                root,
                offsets,
                loc,
                rot,
                usedWorldLocationOffset,
                worldLocationOffset,
                "reject_implausible_position"
            );
            return false;
        }

        if (profile.probeCommonOffsets)
        {
            const double xy = std::sqrt(loc.x * loc.x + loc.y * loc.y);

            if (xy < LIESOFP_MIN_DIRECT_ACTOR_ACCEPT_XY_RADIUS)
            {
                ++stats.originRejects;
                append_candidate_debug_line(
                    "direct_actor",
                    offsetSource,
                    nullptr,
                    pawnLike,
                    root,
                    offsets,
                    loc,
                    rot,
                    usedWorldLocationOffset,
                    worldLocationOffset,
                    "reject_low_xy_static"
                );
                return false;
            }
        }

        append_candidate_debug_line(
            "direct_actor",
            offsetSource,
            nullptr,
            pawnLike,
            root,
            offsets,
            loc,
            rot,
            usedWorldLocationOffset,
            worldLocationOffset,
            "accept"
        );

        out = Snapshot{};
        out.loc = loc;
        out.rot = rot;
        out.controller = nullptr;
        out.pawn = pawnLike;
        out.root = root;
        out.profile = &profile;
        out.effectiveOffsets = offsets;
        out.hasEffectiveOffsets = true;
        out.offsetSource = offsetSource ? offsetSource : "directActor";
        out.usedWorldLocationOffset = usedWorldLocationOffset;
        out.worldLocationOffset = worldLocationOffset;

        // No controller path here. Still try global PlayerCameraManager scan;
        // otherwise camera falls back to player loc + 180, which is fine for XYZ discovery.
        read_camera_with_offsets(profile, offsets, nullptr, nullptr, out);

        ++stats.accepted;
        return true;
    }

    static bool scan_direct_actor_list_for_snapshot(
        const GameProfile& profile,
        const std::vector<UObject*>& objects,
        const char* label,
        Snapshot& out,
        DirectActorScanStats& stats,
        std::ostringstream& debug)
    {
        const auto candidates = build_direct_actor_offset_candidates(profile);
        const size_t maxObjects = std::min<size_t>(objects.size(), 512);

        for (size_t objectIndex = 0; objectIndex < maxObjects; ++objectIndex)
        {
            UObject* obj = objects[objectIndex];
            if (!obj)
                continue;

            ++stats.testedObjects;

            for (size_t ci = 0; ci < candidates.size(); ++ci)
            {
                Snapshot candidate{};
                std::string source = std::string("direct_") + (label ? label : "actor") + "#" + std::to_string(objectIndex) + "/probe#" + std::to_string(ci);

                if (safe_snapshot_from_direct_pawn_like_object(
                    profile,
                    candidates[ci],
                    obj,
                    candidate,
                    source.c_str(),
                    stats))
                {
                    debug
                        << "direct_scan_locked label=" << (label ? label : "actor")
                        << " objectIndex=" << objectIndex
                        << " candidateIndex=" << ci
                        << " pawn=0x" << std::hex << reinterpret_cast<uintptr_t>(candidate.pawn)
                        << " root=0x" << reinterpret_cast<uintptr_t>(candidate.root)
                        << " rootOffset=0x" << candidate.effectiveOffsets.pawnRootComponent
                        << " locOffset=0x" << candidate.effectiveOffsets.sceneRelativeLocation
                        << " rotOffset=0x" << candidate.effectiveOffsets.sceneRelativeRotation;

                    if (candidate.usedWorldLocationOffset)
                        debug << " worldLocOffset=0x" << candidate.worldLocationOffset;

                    debug << std::dec
                        << " loc=" << candidate.loc.x << "," << candidate.loc.y << "," << candidate.loc.z
                        << ";";

                    out = candidate;
                    return true;
                }
            }
        }

        return false;
    }

    static bool find_direct_actor_snapshot_for_profile(const GameProfile& profile, Snapshot& out)
    {
        if (!profile.probeCommonOffsets)
            return false;

        DirectActorScanStats stats{};
        std::ostringstream debug;
        debug << "direct_actor_scan_start;";

        std::vector<UObject*> characters;
        UObjectGlobals::FindAllOf(STR("Character"), characters);
        stats.characterCount = static_cast<int>(characters.size());
        debug << " characters=" << stats.characterCount << ";";

        if (scan_direct_actor_list_for_snapshot(profile, characters, "Character", out, stats, debug))
        {
            debug << " result=accepted;";
            g_lastDiscoveryDebug = debug.str();
            write_text_atomic(CANDIDATE_DEBUG_FILE, g_candidateDebug);
            return true;
        }

        std::vector<UObject*> pawns;
        UObjectGlobals::FindAllOf(STR("Pawn"), pawns);
        stats.pawnCount = static_cast<int>(pawns.size());
        debug << " pawns=" << stats.pawnCount << ";";

        if (scan_direct_actor_list_for_snapshot(profile, pawns, "Pawn", out, stats, debug))
        {
            debug << " result=accepted;";
            g_lastDiscoveryDebug = debug.str();
            write_text_atomic(CANDIDATE_DEBUG_FILE, g_candidateDebug);
            return true;
        }

        debug
            << " result=none"
            << " testedObjects=" << stats.testedObjects
            << " rootFails=" << stats.rootFails
            << " transformFails=" << stats.transformFails
            << " originRejects=" << stats.originRejects
            << ";";

        g_lastDiscoveryDebug = debug.str();
        write_text_atomic(CANDIDATE_DEBUG_FILE, g_candidateDebug);
        return false;
    }

    static bool refresh_direct_actor_snapshot_from_cache(
        const GameProfile& profile,
        const Snapshot& cached,
        Snapshot& out)
    {
        if (!profile.probeCommonOffsets)
            return false;

        if (!cached.pawn || !cached.hasEffectiveOffsets)
            return false;

        if (!is_readable_range(cached.pawn, sizeof(void*)))
            return false;

        DirectActorScanStats stats{};
        std::string source = cached.offsetSource.empty()
            ? std::string("cached_direct_actor")
            : std::string("cached_") + cached.offsetSource;

        Snapshot candidate{};
        if (!safe_snapshot_from_direct_pawn_like_object(
            profile,
            cached.effectiveOffsets,
            cached.pawn,
            candidate,
            source.c_str(),
            stats))
        {
            return false;
        }

        out = candidate;
        return true;
    }

    static bool find_controller_and_snapshot_for_profile(const GameProfile& profile, UObject*& cachedController, Snapshot& out)
    {
        reset_candidate_debug("find_controller_and_snapshot_for_profile");

        std::ostringstream discoveryDebug;
        discoveryDebug << "controller_scan_start;";

        if (cachedController)
        {
            if (safe_snapshot_with_profile(profile, cachedController, out))
            {
                discoveryDebug << " cached_controller_success;";
                g_lastDiscoveryDebug = discoveryDebug.str();
                write_text_atomic(CANDIDATE_DEBUG_FILE, g_candidateDebug);
                return true;
            }

            discoveryDebug << " cached_controller_rejected;";
            cachedController = nullptr;
        }

        std::vector<UObject*> controllers;
        UObjectGlobals::FindAllOf(STR("PlayerController"), controllers);
        discoveryDebug << " controllers=" << controllers.size() << ";";

        for (size_t controllerIndex = 0; controllerIndex < controllers.size(); ++controllerIndex)
        {
            UObject* controller = controllers[controllerIndex];
            if (!controller)
                continue;

            Snapshot candidate{};
            if (safe_snapshot_with_profile(profile, controller, candidate))
            {
                cachedController = controller;
                out = candidate;
                discoveryDebug << " controller_success index=" << controllerIndex << " source=" << candidate.offsetSource << ";";
                g_lastDiscoveryDebug = discoveryDebug.str();
                write_text_atomic(CANDIDATE_DEBUG_FILE, g_candidateDebug);
                return true;
            }
        }

        discoveryDebug << " controller_scan_none;";
        g_lastDiscoveryDebug = discoveryDebug.str();

        // Lies of P fallback: sometimes the PlayerController->Pawn chain is not the right path,
        // or the pawn chain only gives local origin. Try direct Character/Pawn actor objects.
        if (profile.probeCommonOffsets)
        {
            Snapshot direct{};
            if (find_direct_actor_snapshot_for_profile(profile, direct))
            {
                cachedController = nullptr;
                out = direct;
                return true;
            }
        }

        write_text_atomic(CANDIDATE_DEBUG_FILE, g_candidateDebug);
        return false;
    }

    // ============================================================
    // Optional Kismet LineTraceSingle raycast
    // ============================================================

    static UFunction* get_line_trace_single_function()
    {
        static UFunction* function = nullptr;

        // Do not permanently cache a miss. In some games the object is not
        // discoverable during early loading, so retry until it exists.
        if (!function)
        {
            UObject* found = UObjectGlobals::StaticFindObject(
                nullptr,
                nullptr,
                STR("/Script/Engine.KismetSystemLibrary:LineTraceSingle"),
                false
            );

            function = reinterpret_cast<UFunction*>(found);
        }

        return function;
    }

    static UObject* get_kismet_system_library_default_object()
    {
        static UObject* object = nullptr;

        // Same as above: cache hits, but keep retrying misses.
        if (!object)
        {
            object = UObjectGlobals::StaticFindObject(
                nullptr,
                nullptr,
                STR("/Script/Engine.Default__KismetSystemLibrary"),
                false
            );
        }

        return object;
    }

    template <typename T>
    static void write_raw_param(uint8_t* buffer, size_t offset, const T& value)
    {
        std::memcpy(buffer + offset, &value, sizeof(T));
    }

    static bool call_line_trace_single_layout(
        UObject* worldContext,
        const Vec3& start,
        const Vec3& end,
        uint8_t traceTypeQuery,
        size_t hitResultSize,
        bool& outBlocked,
        std::ostringstream* debug = nullptr)
    {
        outBlocked = false;

        if (!worldContext)
            return false;

        UFunction* function = get_line_trace_single_function();
        UObject* kismetCDO = get_kismet_system_library_default_object();

        if (!function || !kismetCDO)
            return false;

        // Important: Gothic may have a different FHitResult size than SH2.
        // Using a big raw param buffer lets ProcessEvent write the return bool
        // even when the real params size is larger than our original SH2 struct.
        alignas(16) uint8_t params[0x500]{};

        constexpr size_t OFF_WorldContext = 0x00;
        constexpr size_t OFF_Start = 0x08;
        constexpr size_t OFF_End = 0x20;
        constexpr size_t OFF_TraceChannel = 0x38;
        constexpr size_t OFF_TraceComplex = 0x39;
        constexpr size_t OFF_ActorsToIgnore = 0x40;
        constexpr size_t OFF_DrawDebugType = 0x50;
        constexpr size_t OFF_OutHit = 0x58;

        const size_t afterHit = OFF_OutHit + hitResultSize;
        const size_t offIgnoreSelf = afterHit;
        const size_t offTraceColor = afterHit + 0x04;
        const size_t offTraceHitColor = afterHit + 0x14;
        const size_t offDrawTime = afterHit + 0x24;
        const size_t offReturnValue = afterHit + 0x28;

        if (offReturnValue + sizeof(bool) >= sizeof(params))
            return false;

        TArrayRaw ignored{};
        LinearColorRaw traceColor{ 0.0f, 1.0f, 0.0f, 1.0f };
        LinearColorRaw hitColor{ 1.0f, 0.0f, 0.0f, 1.0f };
        bool falseBool = false;
        bool trueBool = true;
        uint8_t drawDebugNone = 0;
        float drawTime = 0.0f;

        write_raw_param(params, OFF_WorldContext, worldContext);
        write_raw_param(params, OFF_Start, start);
        write_raw_param(params, OFF_End, end);
        write_raw_param(params, OFF_TraceChannel, traceTypeQuery);
        write_raw_param(params, OFF_TraceComplex, falseBool);
        write_raw_param(params, OFF_ActorsToIgnore, ignored);
        write_raw_param(params, OFF_DrawDebugType, drawDebugNone);
        write_raw_param(params, offIgnoreSelf, trueBool);
        write_raw_param(params, offTraceColor, traceColor);
        write_raw_param(params, offTraceHitColor, hitColor);
        write_raw_param(params, offDrawTime, drawTime);
        write_raw_param(params, offReturnValue, falseBool);

        __try
        {
            kismetCDO->ProcessEvent(function, params);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            if (debug)
                *debug << " h" << std::hex << hitResultSize << std::dec << ":except";
            return false;
        }

        const bool returnValue = params[offReturnValue] != 0;

        // FHitResult usually stores bBlockingHit as the low bit in the first byte.
        // This helps when the bool return offset changed but OutHit is still written.
        const bool outHitBlockingBit = (params[OFF_OutHit] & 0x01) != 0;

        outBlocked = returnValue || outHitBlockingBit;

        if (debug)
        {
            *debug
                << " h0x" << std::hex << hitResultSize << std::dec
                << "(rv=" << (returnValue ? "1" : "0")
                << ",hit0=0x" << std::hex << static_cast<int>(params[OFF_OutHit]) << std::dec
                << ",b=" << (outBlocked ? "1" : "0") << ")";
        }

        return true;
    }

    static bool call_line_trace_single_layout_ue4_float(
        UObject* worldContext,
        const Vec3& start,
        const Vec3& end,
        uint8_t traceTypeQuery,
        size_t hitResultSize,
        bool& outBlocked,
        std::ostringstream* debug = nullptr)
    {
        outBlocked = false;

        if (!worldContext)
            return false;

        UFunction* function = get_line_trace_single_function();
        UObject* kismetCDO = get_kismet_system_library_default_object();

        if (!function || !kismetCDO)
            return false;

        alignas(16) uint8_t params[0x400]{};

        // UE4 float FVector layout for UKismetSystemLibrary::LineTraceSingle.
        constexpr size_t OFF_WorldContext = 0x00;
        constexpr size_t OFF_Start = 0x08;
        constexpr size_t OFF_End = 0x14;
        constexpr size_t OFF_TraceChannel = 0x20;
        constexpr size_t OFF_TraceComplex = 0x21;
        constexpr size_t OFF_ActorsToIgnore = 0x28;
        constexpr size_t OFF_DrawDebugType = 0x38;
        constexpr size_t OFF_OutHit = 0x40;

        const size_t afterHit = OFF_OutHit + hitResultSize;
        const size_t offIgnoreSelf = afterHit;
        const size_t offTraceColor = afterHit + 0x04;
        const size_t offTraceHitColor = afterHit + 0x14;
        const size_t offDrawTime = afterHit + 0x24;
        const size_t offReturnValue = afterHit + 0x28;

        if (offReturnValue + sizeof(bool) >= sizeof(params))
            return false;

        Vec3F startF{
            static_cast<float>(start.x),
            static_cast<float>(start.y),
            static_cast<float>(start.z)
        };

        Vec3F endF{
            static_cast<float>(end.x),
            static_cast<float>(end.y),
            static_cast<float>(end.z)
        };

        TArrayRaw ignored{};
        LinearColorRaw traceColor{ 0.0f, 1.0f, 0.0f, 1.0f };
        LinearColorRaw hitColor{ 1.0f, 0.0f, 0.0f, 1.0f };
        bool falseBool = false;
        bool trueBool = true;
        uint8_t drawDebugNone = 0;
        float drawTime = 0.0f;

        write_raw_param(params, OFF_WorldContext, worldContext);
        write_raw_param(params, OFF_Start, startF);
        write_raw_param(params, OFF_End, endF);
        write_raw_param(params, OFF_TraceChannel, traceTypeQuery);
        write_raw_param(params, OFF_TraceComplex, falseBool);
        write_raw_param(params, OFF_ActorsToIgnore, ignored);
        write_raw_param(params, OFF_DrawDebugType, drawDebugNone);
        write_raw_param(params, offIgnoreSelf, trueBool);
        write_raw_param(params, offTraceColor, traceColor);
        write_raw_param(params, offTraceHitColor, hitColor);
        write_raw_param(params, offDrawTime, drawTime);
        write_raw_param(params, offReturnValue, falseBool);

        __try
        {
            kismetCDO->ProcessEvent(function, params);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            if (debug)
                *debug << " h" << std::hex << hitResultSize << std::dec << ":except";
            return false;
        }

        const bool returnValue = params[offReturnValue] != 0;
        const bool outHitBlockingBit = (params[OFF_OutHit] & 0x01) != 0;

        outBlocked = returnValue || outHitBlockingBit;

        if (debug)
        {
            *debug
                << " h0x" << std::hex << hitResultSize << std::dec
                << "(ue4,rv=" << (returnValue ? "1" : "0")
                << ",hit0=0x" << std::hex << static_cast<int>(params[OFF_OutHit]) << std::dec
                << ",b=" << (outBlocked ? "1" : "0") << ")";
        }

        return true;
    }

    static bool call_line_trace_single_layout_for_profile(
        const GameProfile& profile,
        UObject* worldContext,
        const Vec3& start,
        const Vec3& end,
        uint8_t traceTypeQuery,
        size_t hitResultSize,
        bool& outBlocked,
        std::ostringstream* debug = nullptr)
    {
        if (profile.vectorStorageMode == VectorStorageMode::UE4Float)
        {
            return call_line_trace_single_layout_ue4_float(
                worldContext,
                start,
                end,
                traceTypeQuery,
                hitResultSize,
                outBlocked,
                debug
            );
        }

        return call_line_trace_single_layout(
            worldContext,
            start,
            end,
            traceTypeQuery,
            hitResultSize,
            outBlocked,
            debug
        );
    }

    static std::vector<size_t> trace_hit_result_sizes_for_profile(const GameProfile& profile)
    {
        if (profile.vectorStorageMode == VectorStorageMode::UE4Float)
            return { 0x88, 0x8C, 0x90, 0x98, 0xA0, 0xA8, 0xB0, 0xB8, 0xC0, 0xE8 };

        return { 0xE8, 0xF0, 0xF8, 0x100, 0x108, 0x110, 0x118 };
    }


    static bool line_trace_single(
        UObject* worldContext,
        const Vec3& start,
        const Vec3& end,
        uint8_t traceTypeQuery,
        bool& outBlocked,
        std::ostringstream* debug = nullptr)
    {
        outBlocked = false;

        if (!worldContext)
            return false;

        // SH2 worked with 0xE8. Gothic may use another FHitResult size/layout.
        // We scan a small set of likely UE5 layouts and treat ANY blocking result
        // as blocked. If all valid layouts are clear, the ray is clear.
        static constexpr std::array<size_t, 7> HIT_RESULT_SIZES = {
            0xE8, 0xF0, 0xF8, 0x100, 0x108, 0x110, 0x118
        };

        bool anyExecuted = false;
        bool anyBlocked = false;

        for (size_t hitSize : HIT_RESULT_SIZES)
        {
            bool blocked = false;
            const bool ok = call_line_trace_single_layout(
                worldContext,
                start,
                end,
                traceTypeQuery,
                hitSize,
                blocked,
                debug
            );

            if (!ok)
                continue;

            anyExecuted = true;

            if (blocked)
                anyBlocked = true;
        }

        if (!anyExecuted)
            return false;

        outBlocked = anyBlocked;
        return true;
    }

    static bool trace_visibility_to_point(
        const GameProfile& profile,
        UObject* worldContext,
        const Vec3& cam,
        const Vec3& target,
        bool& outVisible,
        std::ostringstream* debug = nullptr)
    {
        outVisible = false;

        if (!worldContext)
            return false;

        bool anyTraceExecuted = false;
        bool anyBlocked = false;

        const int channelCount = std::max(1, std::min(profile.traceChannelCount, static_cast<int>(profile.traceChannels.size())));

        for (int i = 0; i < channelCount; ++i)
        {
            const uint8_t channel = profile.traceChannels[static_cast<size_t>(i)];

            bool blocked = false;
            const bool traceOk = line_trace_single(worldContext, cam, target, channel, blocked, debug);

            if (debug)
            {
                *debug
                    << " ch" << static_cast<int>(channel)
                    << "=" << (traceOk ? (blocked ? "hit" : "clear") : "fail");
            }

            if (!traceOk)
                continue;

            anyTraceExecuted = true;

            if (blocked)
                anyBlocked = true;
        }

        if (!anyTraceExecuted)
            return false;

        // Multi-channel Gothic behavior:
        // - any hit on tested channels means the line is blocked by world collision
        // - all tested channels clear means that body point is visible
        outVisible = !anyBlocked;
        return true;
    }

    static VisibilityValue compute_remote_visibility(
        const GameProfile& profile,
        UObject* worldContext,
        const Vec3& cam,
        const Vec3& player,
        std::ostringstream* debug = nullptr)
    {
        VisibilityValue result{};

        if (!profile.tryRaycast || !worldContext)
            return result;

        if (!sane_vec(cam) || !sane_vec(player))
            return result;

        if (distance_3d(cam, player) > profile.maxVisibleDistance)
        {
            result.known = true;
            result.visible = false;

            if (debug)
                *debug << "distance_over_limit";

            return result;
        }

        // Body points. We do not trace feet because they often skim collision edges.
        const std::array<Vec3, 5> targets = {
            Vec3{ player.x,        player.y,        player.z + 165.0 }, // head
            Vec3{ player.x,        player.y,        player.z + 120.0 }, // chest
            Vec3{ player.x,        player.y,        player.z + 85.0  }, // belly
            Vec3{ player.x + 25.0, player.y,        player.z + 120.0 }, // right side
            Vec3{ player.x - 25.0, player.y,        player.z + 120.0 }  // left side
        };

        for (size_t i = 0; i < targets.size(); ++i)
        {
            bool visible = false;

            if (debug)
                *debug << " target" << i << ":";

            const bool traceOk = trace_visibility_to_point(
                profile,
                worldContext,
                cam,
                targets[i],
                visible,
                debug
            );

            if (!traceOk)
            {
                if (debug)
                    *debug << " -> invalid;";
                continue;
            }

            ++result.validRays;

            if (visible)
            {
                ++result.cleanRays;
                if (debug)
                    *debug << " -> visible;";
            }
            else
            {
                if (debug)
                    *debug << " -> blocked;";
            }
        }

        // This is the actual hide/show rule:
        // at least N clean body rays -> show sprite
        // otherwise -> hide sprite
        if (result.validRays > 0)
        {
            result.known = true;
            result.visible = result.cleanRays >= profile.visibleRaysRequired;
        }

        return result;
    }

    static std::vector<WorldContextCandidate> collect_world_context_candidates(const Snapshot& snapshot)
    {
        std::vector<WorldContextCandidate> contexts;

        auto add_unique = [&](const char* label, UObject* object)
            {
                if (!object)
                    return;

                for (const auto& c : contexts)
                {
                    if (c.object == object)
                        return;
                }

                contexts.push_back(WorldContextCandidate{ label, object });
            };

        // UWorld is usually the best Kismet world context, so try it first during calibration.
        // Only do this during calibration, not every ray, because FindAllOf("World") is expensive.
        std::vector<UObject*> worlds;
        UObjectGlobals::FindAllOf(STR("World"), worlds);

        int worldCount = 0;
        for (UObject* world : worlds)
        {
            if (!world)
                continue;

            add_unique("world", world);
            ++worldCount;

            if (worldCount >= 3)
                break;
        }

        add_unique("controller", snapshot.controller);
        add_unique("pawn", snapshot.pawn);
        add_unique("root", snapshot.root);
        add_unique("cameraManager", snapshot.cameraManager);

        return contexts;
    }

    static std::array<Vec3, 5> make_body_targets(const Vec3& player)
    {
        return {
            Vec3{ player.x,        player.y,        player.z + 165.0 },
            Vec3{ player.x,        player.y,        player.z + 120.0 },
            Vec3{ player.x,        player.y,        player.z + 85.0  },
            Vec3{ player.x + 25.0, player.y,        player.z + 120.0 },
            Vec3{ player.x - 25.0, player.y,        player.z + 120.0 }
        };
    }

    static bool is_trace_cache_usable()
    {
        if (!g_traceCache.locked || !g_traceCache.worldContext)
            return false;

        if (!is_readable_range(g_traceCache.worldContext, sizeof(void*)))
            return false;

        if (g_traceCache.failedFastCalls > 10)
            return false;

        return true;
    }

    static void reset_trace_cache(const char* reason, std::ostringstream* debug)
    {
        if (debug)
            *debug << "cache_reset=" << (reason ? reason : "unknown") << ";";

        g_traceCache = CachedTraceVariant{};
    }

    static bool try_calibrate_trace_variant(
        const GameProfile& profile,
        const Snapshot& snapshot,
        const std::vector<RemotePlayer>& players,
        std::ostringstream& debug)
    {
        if (is_trace_cache_usable())
            return true;

        g_traceCache = CachedTraceVariant{};

        const auto contexts = collect_world_context_candidates(snapshot);

        const auto hitResultSizes = trace_hit_result_sizes_for_profile(profile);

        debug << "calibration_start contexts=" << contexts.size() << ";";

        bool haveFirstValid = false;
        CachedTraceVariant firstValid{};

        const int channelCount = std::max(1, std::min(profile.traceChannelCount, static_cast<int>(profile.traceChannels.size())));

        for (const RemotePlayer& p : players)
        {
            const double distToLocal = distance_3d(snapshot.loc, p.loc);
            if (distToLocal < SELF_SKIP_DISTANCE)
                continue;

            if (!sane_vec(snapshot.camLoc) || !sane_vec(p.loc))
                continue;

            if (distance_3d(snapshot.camLoc, p.loc) > profile.maxVisibleDistance)
                continue;

            // Calibration uses only the chest ray. That is enough to find the working
            // world context + trace channel + FHitResult layout without nuking FPS.
            const Vec3 chestTarget{ p.loc.x, p.loc.y, p.loc.z + 120.0 };

            for (const WorldContextCandidate& ctx : contexts)
            {
                if (!ctx.object)
                    continue;

                for (int ci = 0; ci < channelCount; ++ci)
                {
                    const uint8_t channel = profile.traceChannels[static_cast<size_t>(ci)];

                    for (size_t hitSize : hitResultSizes)
                    {
                        bool blocked = false;
                        std::ostringstream tiny;
                        const bool ok = call_line_trace_single_layout_for_profile(
                            profile,
                            ctx.object,
                            snapshot.camLoc,
                            chestTarget,
                            channel,
                            hitSize,
                            blocked,
                            &tiny
                        );

                        if (!ok)
                            continue;

                        if (!haveFirstValid)
                        {
                            firstValid.locked = true;
                            firstValid.worldContext = ctx.object;
                            firstValid.label = ctx.label ? ctx.label : "unknown";
                            firstValid.channel = channel;
                            firstValid.hitResultSize = hitSize;
                            firstValid.chosenFromBlockingHit = false;
                            haveFirstValid = true;
                        }

                        // Best case: while testing behind cover, we find a variant that actually hits.
                        // Cache this and stop scanning immediately.
                        if (blocked)
                        {
                            g_traceCache.locked = true;
                            g_traceCache.worldContext = ctx.object;
                            g_traceCache.label = ctx.label ? ctx.label : "unknown";
                            g_traceCache.channel = channel;
                            g_traceCache.hitResultSize = hitSize;
                            g_traceCache.chosenFromBlockingHit = true;
                            g_traceCache.failedFastCalls = 0;

                            debug
                                << " calibration_locked_blocking"
                                << " ctx=" << g_traceCache.label
                                << "@0x" << std::hex << reinterpret_cast<uintptr_t>(g_traceCache.worldContext) << std::dec
                                << " ch=" << static_cast<int>(g_traceCache.channel)
                                << " hitSize=0x" << std::hex << g_traceCache.hitResultSize << std::dec
                                << " player=" << p.name
                                << ";";

                            return true;
                        }
                    }
                }
            }
        }

        // If the player is currently not behind cover, no tested ray may block.
        // Still cache the first valid variant so normal visible case is cheap.
        if (haveFirstValid)
        {
            g_traceCache = firstValid;
            debug
                << " calibration_locked_first_valid"
                << " ctx=" << g_traceCache.label
                << "@0x" << std::hex << reinterpret_cast<uintptr_t>(g_traceCache.worldContext) << std::dec
                << " ch=" << static_cast<int>(g_traceCache.channel)
                << " hitSize=0x" << std::hex << g_traceCache.hitResultSize << std::dec
                << ";";
            return true;
        }

        debug << " calibration_failed_no_valid_variant;";
        return false;
    }

    static bool trace_visibility_to_point_fast(
        const GameProfile& profile,
        const Vec3& cam,
        const Vec3& target,
        bool& outVisible,
        std::ostringstream* debug = nullptr)
    {
        outVisible = false;

        if (!is_trace_cache_usable())
            return false;

        bool blocked = false;
        const bool ok = call_line_trace_single_layout_for_profile(
            profile,
            g_traceCache.worldContext,
            cam,
            target,
            g_traceCache.channel,
            g_traceCache.hitResultSize,
            blocked,
            debug
        );

        if (!ok)
        {
            ++g_traceCache.failedFastCalls;
            return false;
        }

        g_traceCache.failedFastCalls = 0;

        if (blocked)
        {
            outVisible = false;
            return true;
        }

        // Foliage/tree rescue: walls may calibrate to one working channel, while foliage
        // can respond on a different trace channel. Only run this when the cached ray is clear,
        // and reuse the already-working context + hit layout. Cheap enough: max 7 extra calls per body ray.
        if (ENABLE_FOLIAGE_CHANNEL_RESCUE)
        {
            bool anyExtraExecuted = false;
            bool anyExtraBlocked = false;

            for (uint8_t ch = 0; ch <= 7; ++ch)
            {
                if (ch == g_traceCache.channel)
                    continue;

                bool extraBlocked = false;
                const bool extraOk = call_line_trace_single_layout_for_profile(
                    profile,
                    g_traceCache.worldContext,
                    cam,
                    target,
                    ch,
                    g_traceCache.hitResultSize,
                    extraBlocked,
                    nullptr
                );

                if (!extraOk)
                    continue;

                anyExtraExecuted = true;

                if (extraBlocked)
                {
                    anyExtraBlocked = true;
                    if (debug)
                        *debug << " foliageRescue=hitCh" << static_cast<int>(ch);
                    break;
                }
            }

            if (anyExtraExecuted && anyExtraBlocked)
            {
                outVisible = false;
                return true;
            }
        }

        outVisible = true;
        return true;
    }

    static VisibilityValue compute_remote_visibility_fast(
        const GameProfile& profile,
        const Snapshot& snapshot,
        const Vec3& player,
        std::ostringstream* debug = nullptr)
    {
        VisibilityValue result{};

        if (!profile.tryRaycast)
            return result;

        if (!sane_vec(snapshot.camLoc) || !sane_vec(player))
            return result;

        if (distance_3d(snapshot.camLoc, player) > profile.maxVisibleDistance)
        {
            result.known = true;
            result.visible = false;
            if (debug)
                *debug << "distance_over_limit";
            return result;
        }

        const auto targets = make_body_targets(player);

        // Fast mode uses 3 stable body points instead of all 5.
        static constexpr std::array<size_t, 3> FAST_TARGET_INDICES = { 0, 1, 2 };

        for (size_t targetIndex : FAST_TARGET_INDICES)
        {
            bool visible = false;

            if (debug)
                *debug << " target" << targetIndex << ":";

            const bool ok = trace_visibility_to_point_fast(
                profile,
                snapshot.camLoc,
                targets[targetIndex],
                visible,
                debug
            );

            if (!ok)
            {
                if (debug)
                    *debug << " -> invalid;";
                continue;
            }

            ++result.validRays;

            if (visible)
            {
                ++result.cleanRays;
                if (debug)
                    *debug << " -> visible;";
            }
            else
            {
                if (debug)
                    *debug << " -> blocked;";
            }
        }

        if (result.validRays > 0)
        {
            result.known = true;
            result.visible = result.cleanRays >= FAST_VISIBLE_RAYS_REQUIRED;
        }

        return result;
    }

    static std::string make_visibility_json(const Snapshot& snapshot, std::string& raycastStatus)
    {
        const GameProfile* profile = snapshot.profile;

        std::ostringstream debug;
        debug << std::fixed << std::setprecision(6);
        debug << "build=" << BUILD_TAG << "\n";
        debug << "profile=" << (profile ? profile->id : "null") << "\n";
        debug << "vector_storage=" << (profile && profile->vectorStorageMode == VectorStorageMode::UE4Float ? "UE4Float" : "UE5Double") << "\n";
        debug << "offset_source=" << snapshot.offsetSource << "\n";
        if (snapshot.usedWorldLocationOffset)
            debug << "world_location_offset=0x" << std::hex << snapshot.worldLocationOffset << std::dec << "\n";
        debug << "local=" << snapshot.loc.x << ", " << snapshot.loc.y << ", " << snapshot.loc.z << "\n";
        debug << "camera=" << snapshot.camLoc.x << ", " << snapshot.camLoc.y << ", " << snapshot.camLoc.z << "\n";

        if (!profile)
        {
            raycastStatus = "no_profile";
            debug << "status=no_profile\n";
            if (WRITE_RAYCAST_DEBUG_FILE)
                write_text_atomic(RAYCAST_DEBUG_FILE, debug.str());
            return "{}";
        }

        if (!profile->tryRaycast)
        {
            raycastStatus = "raycast_disabled_for_profile";
            debug << "status=raycast_disabled_for_profile\n";
            if (WRITE_RAYCAST_DEBUG_FILE)
                write_text_atomic(RAYCAST_DEBUG_FILE, debug.str());
            return "{}";
        }

        const auto players = read_remote_players();
        debug << "remote_players_count=" << players.size() << "\n";

        if (players.empty())
        {
            raycastStatus = "no_remote_players";
            debug << "status=no_remote_players\n";
            if (WRITE_RAYCAST_DEBUG_FILE)
                write_text_atomic(RAYCAST_DEBUG_FILE, debug.str());
            return "{}";
        }

        UFunction* traceFn = get_line_trace_single_function();
        UObject* kismetCDO = get_kismet_system_library_default_object();

        debug << "line_trace_function=0x" << std::hex << reinterpret_cast<uintptr_t>(traceFn) << "\n";
        debug << "kismet_cdo=0x" << reinterpret_cast<uintptr_t>(kismetCDO) << "\n";
        debug << "world_context_controller=0x" << reinterpret_cast<uintptr_t>(snapshot.controller) << "\n";
        debug << "world_context_pawn=0x" << reinterpret_cast<uintptr_t>(snapshot.pawn) << "\n";
        debug << "camera_manager_used=0x" << reinterpret_cast<uintptr_t>(snapshot.cameraManager) << "\n";
        debug << "camera_fallback_like=" << ((distance_3d(snapshot.loc, snapshot.camLoc) < 181.0 && distance_3d(snapshot.loc, snapshot.camLoc) > 179.0) ? "true" : "false") << "\n";
        debug << "foliage_channel_rescue=" << (ENABLE_FOLIAGE_CHANNEL_RESCUE ? "true" : "false") << "\n";
        debug << "trace_cache_locked=" << (g_traceCache.locked ? "true" : "false") << "\n";
        if (g_traceCache.locked)
        {
            debug << "trace_cache_label=" << g_traceCache.label << "\n";
            debug << "trace_cache_object=0x" << reinterpret_cast<uintptr_t>(g_traceCache.worldContext) << "\n";
            debug << std::dec;
            debug << "trace_cache_channel=" << static_cast<int>(g_traceCache.channel) << "\n";
            debug << std::hex;
            debug << "trace_cache_hit_size=0x" << g_traceCache.hitResultSize << "\n";
            debug << std::dec;
            debug << "trace_cache_chosen_from_block=" << (g_traceCache.chosenFromBlockingHit ? "true" : "false") << "\n";
        }
        debug << "\n";
        debug << std::dec;

        if (!traceFn || !kismetCDO)
        {
            raycastStatus = "line_trace_function_or_kismet_cdo_missing";
            debug << "status=line_trace_function_or_kismet_cdo_missing\n";
            if (WRITE_RAYCAST_DEBUG_FILE)
                write_text_atomic(RAYCAST_DEBUG_FILE, debug.str());
            return "{}";
        }

        std::ostringstream json;
        json << "{\n";

        bool wroteAny = false;
        int knownPlayers = 0;
        int skippedLocalLikePlayers = 0;

        for (const RemotePlayer& p : players)
        {
            const double distToLocal = distance_3d(snapshot.loc, p.loc);

            // Your remote_players.txt currently contains the local player too.
            // Do not raycast/write the local player as a remote sprite.
            if (distToLocal < SELF_SKIP_DISTANCE)
            {
                ++skippedLocalLikePlayers;
                debug << p.name
                    << " | skipped_self_like=true"
                    << " | remote=" << p.loc.x << ", " << p.loc.y << ", " << p.loc.z
                    << " | distToLocal=" << distToLocal
                    << "\n";
                continue;
            }

            std::ostringstream perPlayerTraceDebug;

            if (!try_calibrate_trace_variant(*profile, snapshot, players, perPlayerTraceDebug))
            {
                perPlayerTraceDebug << " no_cached_trace_variant;";
            }

            if (is_trace_cache_usable())
            {
                perPlayerTraceDebug
                    << " fast_cache=" << g_traceCache.label
                    << "@0x" << std::hex << reinterpret_cast<uintptr_t>(g_traceCache.worldContext) << std::dec
                    << " ch=" << static_cast<int>(g_traceCache.channel)
                    << " hitSize=0x" << std::hex << g_traceCache.hitResultSize << std::dec
                    << " chosenFromBlock=" << (g_traceCache.chosenFromBlockingHit ? "true" : "false")
                    << ";";
            }

            VisibilityValue v = compute_remote_visibility_fast(
                *profile,
                snapshot,
                p.loc,
                &perPlayerTraceDebug
            );

            if (!v.known && g_traceCache.locked)
            {
                reset_trace_cache("fast_trace_failed", &perPlayerTraceDebug);
            }

            debug << p.name
                << " | remote=" << p.loc.x << ", " << p.loc.y << ", " << p.loc.z
                << " | distCam=" << distance_3d(snapshot.camLoc, p.loc)
                << " | known=" << (v.known ? "true" : "false")
                << " | visible=" << (v.visible ? "true" : "false")
                << " | cleanRays=" << v.cleanRays
                << " | validRays=" << v.validRays
                << " | trace={" << perPlayerTraceDebug.str() << "}"
                << "\n";

            if (!v.known)
                continue;

            ++knownPlayers;

            if (wroteAny)
                json << ",\n";

            json << "    \"" << json_escape(p.name) << "\": " << (v.visible ? "true" : "false");
            wroteAny = true;
        }

        json << "\n  }";

        debug << "\nknownPlayers=" << knownPlayers << "\n";
        debug << "skippedLocalLikePlayers=" << skippedLocalLikePlayers << "\n";

        if (WRITE_RAYCAST_DEBUG_FILE)
            write_text_atomic(RAYCAST_DEBUG_FILE, debug.str());

        if (!wroteAny)
        {
            raycastStatus = knownPlayers == 0 ? "no_known_remote_visibility" : "no_remote_players_written";
            return "{}";
        }

        raycastStatus = "ok";
        return json.str();
    }

    // ============================================================
    // JSON/debug writer
    // ============================================================

    static std::string make_json(const Snapshot& s)
    {
        std::string raycastStatus;
        const std::string visibilityJson = make_visibility_json(s, raycastStatus);

        std::ostringstream ss;
        ss << std::fixed << std::setprecision(6);
        ss << "{\n";
        ss << "  \"username\": \"NativePlayer\",\n";
        ss << "  \"build\": \"" << BUILD_TAG << "\",\n";
        ss << "  \"profile\": \"" << json_escape(s.profile ? s.profile->id : "unknown") << "\",\n";
        ss << "  \"raycast_status\": \"" << json_escape(raycastStatus) << "\",\n";
        ss << "  \"x\": " << s.loc.x << ",\n";
        ss << "  \"y\": " << s.loc.y << ",\n";
        ss << "  \"z\": " << s.loc.z << ",\n";
        ss << "  \"pitch\": " << s.rot.pitch << ",\n";
        ss << "  \"yaw\": " << s.rot.yaw << ",\n";
        ss << "  \"roll\": " << s.rot.roll << ",\n";
        ss << "  \"cam_x\": " << s.camLoc.x << ",\n";
        ss << "  \"cam_y\": " << s.camLoc.y << ",\n";
        ss << "  \"cam_z\": " << s.camLoc.z << ",\n";
        ss << "  \"cam_pitch\": " << s.camRot.pitch << ",\n";
        ss << "  \"cam_yaw\": " << s.camRot.yaw << ",\n";
        ss << "  \"cam_roll\": " << s.camRot.roll << ",\n";
        ss << "  \"fov\": " << static_cast<double>(s.fov) << ",\n";
        ss << "  \"visible\": " << visibilityJson << "\n";
        ss << "}\n";
        return ss.str();
    }

    static std::string make_status_json(
        const std::string& exeName,
        const std::string& profileSwitch,
        const GameProfile* preferredProfile,
        const char* status
    )
    {
        std::ostringstream ss;
        ss << "{\n";
        ss << "  \"username\": \"NativePlayer\",\n";
        ss << "  \"build\": \"" << BUILD_TAG << "\",\n";
        ss << "  \"profile\": \"" << json_escape(preferredProfile ? preferredProfile->id : "unknown") << "\",\n";
        ss << "  \"profileSwitch\": \"" << json_escape(profileSwitch) << "\",\n";
        ss << "  \"exe\": \"" << json_escape(exeName) << "\",\n";
        ss << "  \"raycast_status\": \"" << json_escape(status ? status : "status") << "\",\n";
        ss << "  \"x\": 0,\n";
        ss << "  \"y\": 0,\n";
        ss << "  \"z\": 0,\n";
        ss << "  \"pitch\": 0,\n";
        ss << "  \"yaw\": 0,\n";
        ss << "  \"roll\": 0,\n";
        ss << "  \"cam_x\": 0,\n";
        ss << "  \"cam_y\": 0,\n";
        ss << "  \"cam_z\": 0,\n";
        ss << "  \"cam_pitch\": 0,\n";
        ss << "  \"cam_yaw\": 0,\n";
        ss << "  \"cam_roll\": 0,\n";
        ss << "  \"fov\": 90,\n";
        ss << "  \"visible\": {}\n";
        ss << "}\n";
        return ss.str();
    }

    static std::string make_status_debug_text(
        const std::string& exeName,
        const std::string& profileSwitch,
        const GameProfile* preferredProfile,
        const char* status
    )
    {
        std::ostringstream ss;
        ss << "build=" << BUILD_TAG << "\n";
        ss << "exe=" << exeName << "\n";
        ss << "profileSwitch=" << profileSwitch << "\n";
        ss << "profile=" << (preferredProfile ? preferredProfile->id : "unknown") << " / "
            << (preferredProfile ? preferredProfile->displayName : "unknown") << "\n";
        ss << "status=" << (status ? status : "status") << "\n";
        ss << "note=DLL is loaded, but no valid controller/pawn/root snapshot has been found yet.\n";
        ss << "next=If this stays forever in Lies of P, UE4SS object access is not ready or offsets/probe did not find a valid pawn/root.\n";
        ss << "lastDiscoveryDebug=" << g_lastDiscoveryDebug << "\n";
        ss << "candidateDebugFile=" << CANDIDATE_DEBUG_FILE << "\n";
        ss << "candidateDebugLines=" << g_candidateDebugLineCount << "\n";
        return ss.str();
    }

    static std::string make_debug_text(const Snapshot& s, const std::string& exeName, const std::string& profileSwitch)
    {
        std::ostringstream ss;
        ss << std::fixed << std::setprecision(6);
        ss << "build=" << BUILD_TAG << "\n";
        ss << "exe=" << exeName << "\n";
        ss << "profileSwitch=" << profileSwitch << "\n";
        ss << "profile=" << (s.profile ? s.profile->id : "unknown") << " / "
            << (s.profile ? s.profile->displayName : "unknown") << "\n";
        ss << "vectorStorage=" << (s.profile && s.profile->vectorStorageMode == VectorStorageMode::UE4Float ? "UE4Float" : "UE5Double") << "\n";
        ss << "offsetSource=" << s.offsetSource << "\n";
        ss << "lastDiscoveryDebug=" << g_lastDiscoveryDebug << "\n";
        ss << "candidateDebugFile=" << CANDIDATE_DEBUG_FILE << "\n";
        ss << "candidateDebugLines=" << g_candidateDebugLineCount << "\n";

        if (s.hasEffectiveOffsets)
        {
            ss << std::hex;
            ss << "effective.controllerPawn=0x" << s.effectiveOffsets.controllerPawn << "\n";
            ss << "effective.controllerAcknowledgedPawn=0x" << s.effectiveOffsets.controllerAcknowledgedPawn << "\n";
            ss << "effective.controllerCameraManager=0x" << s.effectiveOffsets.controllerCameraManager << "\n";
            ss << "effective.pawnRootComponent=0x" << s.effectiveOffsets.pawnRootComponent << "\n";
            ss << "effective.sceneRelativeLocation=0x" << s.effectiveOffsets.sceneRelativeLocation << "\n";
            ss << "effective.sceneRelativeRotation=0x" << s.effectiveOffsets.sceneRelativeRotation << "\n";
            if (s.usedWorldLocationOffset)
                ss << "effective.worldLocationOffset=0x" << s.worldLocationOffset << "\n";
            ss << "effective.cameraCachePrivate=0x" << s.effectiveOffsets.cameraCachePrivate << "\n";
            ss << "effective.cameraCachePOV=0x" << s.effectiveOffsets.cameraCachePOV << "\n";
            ss << "effective.povLocation=0x" << s.effectiveOffsets.povLocation << "\n";
            ss << "effective.povRotation=0x" << s.effectiveOffsets.povRotation << "\n";
            ss << "effective.povFOV=0x" << s.effectiveOffsets.povFOV << "\n";
            ss << std::dec;
        }

        ss << "\n";

        ss << std::hex;
        ss << "controller=0x" << reinterpret_cast<uintptr_t>(s.controller) << "\n";
        ss << "pawn=0x" << reinterpret_cast<uintptr_t>(s.pawn) << "\n";
        ss << "root=0x" << reinterpret_cast<uintptr_t>(s.root) << "\n";
        ss << "cameraManager=0x" << reinterpret_cast<uintptr_t>(s.cameraManager) << "\n";
        ss << "povAddress=0x" << s.povAddress << "\n";
        ss << "lineTraceFunction=0x" << reinterpret_cast<uintptr_t>(get_line_trace_single_function()) << "\n";
        ss << "kismetCDO=0x" << reinterpret_cast<uintptr_t>(get_kismet_system_library_default_object()) << "\n\n";
        ss << std::dec;

        ss << "player loc=" << s.loc.x << ", " << s.loc.y << ", " << s.loc.z << "\n";
        ss << "player rot=" << s.rot.pitch << ", " << s.rot.yaw << ", " << s.rot.roll << "\n";
        ss << "camera loc=" << s.camLoc.x << ", " << s.camLoc.y << ", " << s.camLoc.z << "\n";
        ss << "camera rot=" << s.camRot.pitch << ", " << s.camRot.yaw << ", " << s.camRot.roll << "\n";
        ss << "fov=" << s.fov << "\n";

        return ss.str();
    }

    // ============================================================
    // Main mod
    // ============================================================

    class GhostCoopNativeUniversal : public CppUserModBase
    {
    public:
        GhostCoopNativeUniversal()
        {
            ModVersion = STR("1.3.2-universal-gothic-sh2-liesofp-worldloc-direct-scan");
            ModName = STR("GhostCoopUniversal");
            ModAuthors = STR("GhostCoop");
            ModDescription = STR("Universal profile based native position writer with optional raycast visibility");

            startTime = Clock::now();
            CreateDirectoryA(BRIDGE_DIR, nullptr);
            write_text_atomic("C:\\GhostCoopBridge\\ghostcoop_loaded_marker.txt", std::string(BUILD_TAG) + "\n");

            exeName = exe_name_lower();
            profileSwitch = to_lower(trim(ACTIVE_GAME_PROFILE));
            preferredProfile = nullptr;

            if (profileSwitch == "auto")
            {
                preferredProfile = find_profile_by_text(exeName);
            }
            else
            {
                preferredProfile = find_profile_by_id(profileSwitch);
            }

            if (preferredProfile)
            {
                Output::send<LogLevel::Warning>(
                    STR("[GhostCoopUniversal]: ACTIVE_GAME_PROFILE selected.\n")
                );
            }
            else
            {
                Output::send<LogLevel::Warning>(
                    STR("[GhostCoopUniversal]: ACTIVE_GAME_PROFILE did not match any profile.\n")
                );
            }

            write_text_atomic(OUT_FILE, make_status_json(exeName, profileSwitch, preferredProfile, "dll_loaded_waiting_for_snapshot"));
            if (WRITE_DEBUG_FILE)
                write_text_atomic(DEBUG_FILE, make_status_debug_text(exeName, profileSwitch, preferredProfile, "dll_loaded_waiting_for_snapshot"));

            // IMPORTANT:
            // Do NOT call UE4SS / UObjectGlobals from a custom worker thread.
            // Lies of P + UE4SS 3.0.1 can crash inside UE4SS if FindAllOf/StaticFindObject
            // is used off the game/update thread while UE4SS is still pattern-scanning.
            // Snapshot discovery stays in on_update(), which is the safe UE4SS callback path.
        }

        ~GhostCoopNativeUniversal() override = default;

        auto on_update() -> void override
        {
            const auto now = Clock::now();
            const bool isLiesOfP = profileSwitch == "liesofp";
            const bool hasDirectActorCache = isLiesOfP
                && hasCachedDirectActorSnapshot
                && cachedDirectActorSnapshot.pawn != nullptr
                && cachedDirectActorSnapshot.profile != nullptr;
            const bool discoveryMode = !activeProfile || (!cachedController && !hasDirectActorCache);

            // No old 4-minute Lies of P delay anymore. Custom UE4SS now initializes
            // correctly, so let Lies of P start probing immediately on the update thread.
            // Known Gothic/SH2 profiles keep the old short boot delay.
            if (!isLiesOfP && now - startTime < BOOT_DELAY)
                return;

            if (now - lastSample < SAMPLE_INTERVAL)
                return;

            lastSample = now;

            // Lies of P offset probing is intentionally heavier than known Gothic/SH2 paths.
            // While we still do not have a valid controller/pawn/root, run discovery only every
            // few seconds instead of every frame/50ms. Once a snapshot is found, normal 50ms
            // position updates resume.
            if (isLiesOfP && discoveryMode)
            {
                if (now - lastControllerSearch < LIESOFP_DISCOVERY_INTERVAL)
                    return;
                lastControllerSearch = now;
            }

            Snapshot snap{};
            bool ok = false;

            // 1) Existing direct actor cache for Lies of P.
            // This avoids repeated UObjectGlobals::FindAllOf("Character"/"Pawn") every few seconds
            // after the first good direct actor candidate is found.
            if (activeProfile && hasDirectActorCache)
            {
                ok = refresh_direct_actor_snapshot_from_cache(*activeProfile, cachedDirectActorSnapshot, snap);
                if (!ok)
                    hasCachedDirectActorSnapshot = false;
            }

            // 2) Existing active profile + cached controller.
            if (!ok && activeProfile && cachedController)
            {
                ok = find_controller_and_snapshot_for_profile(*activeProfile, cachedController, snap);
            }

            // 3) Preferred profile from override/exe.
            if (!ok && preferredProfile)
            {
                UObject*& cache = cachedControllers[profile_index(*preferredProfile)];
                ok = find_controller_and_snapshot_for_profile(*preferredProfile, cache, snap);
            }

            // 4) Optional fallback. Default je vypnutý, aby explicitný switch
            //    "sh2" nikdy omylom neprepol na Gothic profil alebo opačne.
            if (!ok && ALLOW_PROFILE_AUTODETECT_WHEN_SWITCH_FAILS)
            {
                if (now - lastControllerSearch < CONTROLLER_SEARCH_INTERVAL)
                    return;

                lastControllerSearch = now;
                ok = try_all_profiles(snap);
            }

            if (!ok || !snap.profile)
            {
                const bool forceStatusHeartbeat = now - lastNoSnapshotWrite > HEARTBEAT_INTERVAL;
                if (forceStatusHeartbeat)
                {
                    lastNoSnapshotWrite = now;
                    write_text_atomic(OUT_FILE, make_status_json(exeName, profileSwitch, preferredProfile, "no_snapshot_retrying_on_update_thread"));
                    if (WRITE_DEBUG_FILE)
                        write_text_atomic(DEBUG_FILE, make_status_debug_text(exeName, profileSwitch, preferredProfile, "no_snapshot_retrying_on_update_thread"));
                }
                return;
            }

            activeProfile = snap.profile;
            cachedController = snap.controller;

            if (isLiesOfP && snap.profile && snap.controller == nullptr && snap.pawn != nullptr && snap.hasEffectiveOffsets)
            {
                cachedDirectActorSnapshot = snap;
                hasCachedDirectActorSnapshot = true;
            }
            else if (snap.controller != nullptr)
            {
                hasCachedDirectActorSnapshot = false;
            }

            const bool forceHeartbeat = now - lastForcedWrite > HEARTBEAT_INTERVAL;

            if (hasLastSnapshot && !materially_changed(lastSnapshot, snap) && !forceHeartbeat)
                return;

            lastSnapshot = snap;
            hasLastSnapshot = true;
            lastForcedWrite = now;

            write_text_atomic(OUT_FILE, make_json(snap));

            if (WRITE_DEBUG_FILE)
                write_text_atomic(DEBUG_FILE, make_debug_text(snap, exeName, profileSwitch));
        }

    private:
        using Clock = std::chrono::steady_clock;

        // No custom worker thread here on purpose.
        // All UObject/UE4SS access must happen from on_update().
        // A background thread may write plain status files in the future,
        // but it must never call UObjectGlobals::FindAllOf or StaticFindObject.

        Clock::time_point startTime{};
        Clock::time_point lastSample{};
        Clock::time_point lastControllerSearch{};
        Clock::time_point lastForcedWrite{};
        Clock::time_point lastNoSnapshotWrite{};


        std::string exeName;
        std::string profileSwitch;

        const GameProfile* preferredProfile = nullptr;
        const GameProfile* activeProfile = nullptr;
        UObject* cachedController = nullptr;
        std::array<UObject*, std::size(PROFILES)> cachedControllers{};

        Snapshot cachedDirectActorSnapshot{};
        bool hasCachedDirectActorSnapshot = false;

        Snapshot lastSnapshot{};
        bool hasLastSnapshot = false;

        static size_t profile_index(const GameProfile& profile)
        {
            for (size_t i = 0; i < std::size(PROFILES); ++i)
            {
                if (&PROFILES[i] == &profile)
                    return i;
            }

            return 0;
        }

        bool try_all_profiles(Snapshot& out)
        {
            for (size_t i = 0; i < std::size(PROFILES); ++i)
            {
                const GameProfile& p = PROFILES[i];

                // If preferred exists and already failed this frame, skip it here.
                if (preferredProfile && &p == preferredProfile)
                    continue;

                if (find_controller_and_snapshot_for_profile(p, cachedControllers[i], out))
                    return true;
            }

            // If no preferred profile was set, try Gothic first as a final normal path.
            // This keeps behavior deterministic when exe detection is weird.
            if (!preferredProfile && std::size(PROFILES) > 0)
            {
                if (find_controller_and_snapshot_for_profile(PROFILES[0], cachedControllers[0], out))
                    return true;
            }

            return false;
        }
    };
}

#define GHOSTCOOP_API __declspec(dllexport)

extern "C"
{
    GHOSTCOOP_API RC::CppUserModBase* start_mod()
    {
        return new GhostCoopUniversal::GhostCoopNativeUniversal();
    }

    GHOSTCOOP_API void uninstall_mod(RC::CppUserModBase* mod)
    {
        delete mod;
    }
}
