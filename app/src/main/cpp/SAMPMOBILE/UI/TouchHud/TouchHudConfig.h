#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>

namespace touch_hud {

// Exact 1:1 mirror of Game/Pad.h ePadKeys (indices 0..17) so TouchHudCore stays
// completely engine-independent and testable on host without GTA headers.
enum class SampKey : uint8_t {
	Action          = 0,  // TAB (OnFoot) / LCTRL (InCar)         | bit 1
	Crouch          = 1,  // C (OnFoot) / H Horn (InCar)          | bit 2
	Fire            = 2,  // LMB/LCTRL (OnFoot) / LALT (InCar)    | bit 4
	Sprint          = 3,  // SPACE (OnFoot) / W (InCar)           | bit 8
	SecondaryAttack = 4,  // F/ENTER (OnFoot & InCar)             | bit 16
	Jump            = 5,  // LSHIFT (OnFoot) / S (InCar)          | bit 32
	LookRight       = 6,  // E (Weapon next / InCar look right)   | bit 64
	Handbrake       = 7,  // RMB Aim/Block (OnFoot) / SPACE (Car) | bit 128
	LookLeft        = 8,  // Q (Weapon prev / InCar look left)    | bit 256
	Submission      = 9,  // 2 / Plane Landing Gear               | bit 512
	Walk            = 10, // LALT Slow-walk (OnFoot)              | bit 1024
	AnalogUp        = 11, // NUM8                                 | bit 2048
	AnalogDown      = 12, // NUM2                                 | bit 4096
	AnalogLeft      = 13, // NUM4                                 | bit 8192
	AnalogRight     = 14, // NUM6                                 | bit 16384
	Yes             = 15, // Y (ext=1)                            | 65536
	No              = 16, // N (ext=2)                            | 131072
	CtrlBack        = 17, // H (ext=3)                            | 262144
	Count           = 18
};

constexpr uint32_t keyBit(SampKey k)
{
	return 1u << static_cast<uint8_t>(k);
}

enum class ControlId : uint8_t {
	// --- On-Foot page ---
	Joystick = 0,
	FireRight,
	FireLeft,
	Aim,
	Sprint,
	Jump,
	Crouch,
	InteractF,
	PassengerG,
	WeaponPrevQ,
	WeaponNextE,
	CamZoom,
	RpStrip,

	// --- In-Vehicle page ---
	VehSteerLeft,
	VehSteerRight,
	VehJoystick,
	VehAccel,
	VehBrake,
	VehHandbrake,
	VehFire,
	VehHorn,
	VehLookLeftQ,
	VehLookRightE,
	VehExitF,
	VehSubMission2,
	VehDriveBy,
	VehPitchUp,
	VehPitchDown,
	VehFlare,
	VehCamToggle,

	Count
};

constexpr size_t kControlCount = static_cast<size_t>(ControlId::Count);
constexpr size_t kMaxCustomButtons = 8;

inline bool isVehicleControl(ControlId id)
{
	return static_cast<uint8_t>(id) >= static_cast<uint8_t>(ControlId::VehSteerLeft);
}

inline const char* controlConfigKey(ControlId id)
{
	switch (id) {
		case ControlId::Joystick:       return "joystick";
		case ControlId::FireRight:      return "fire_right";
		case ControlId::FireLeft:       return "fire_left";
		case ControlId::Aim:            return "aim";
		case ControlId::Sprint:         return "sprint";
		case ControlId::Jump:           return "jump";
		case ControlId::Crouch:         return "crouch";
		case ControlId::InteractF:      return "interact_f";
		case ControlId::PassengerG:     return "passenger_g";
		case ControlId::WeaponPrevQ:    return "weapon_q";
		case ControlId::WeaponNextE:    return "weapon_e";
		case ControlId::CamZoom:        return "cam_zoom";
		case ControlId::RpStrip:        return "rp_strip";
		case ControlId::VehSteerLeft:   return "veh_steer_left";
		case ControlId::VehSteerRight:  return "veh_steer_right";
		case ControlId::VehJoystick:    return "veh_joystick";
		case ControlId::VehAccel:       return "veh_accel";
		case ControlId::VehBrake:       return "veh_brake";
		case ControlId::VehHandbrake:   return "veh_handbrake";
		case ControlId::VehFire:        return "veh_fire";
		case ControlId::VehHorn:        return "veh_horn";
		case ControlId::VehLookLeftQ:   return "veh_look_q";
		case ControlId::VehLookRightE:  return "veh_look_e";
		case ControlId::VehExitF:       return "veh_exit_f";
		case ControlId::VehSubMission2: return "veh_sub_2";
		case ControlId::VehDriveBy:     return "veh_driveby";
		case ControlId::VehPitchUp:     return "veh_pitch_up";
		case ControlId::VehPitchDown:   return "veh_pitch_down";
		case ControlId::VehFlare:       return "veh_flare";
		case ControlId::VehCamToggle:   return "veh_cam_toggle";
		default:                        return "unknown";
	}
}

enum class AimMode : uint8_t {
	Toggle = 0,
	Hold   = 1,
};

enum class JoystickMode : uint8_t {
	Floating = 0, // PUBG-style dynamic anchor on left half
	Fixed    = 1,
};

enum class LeftFireMode : uint8_t {
	Always  = 0,
	AimOnly = 1,
	Hidden  = 2,
};

enum class VehicleSteeringMode : uint8_t {
	Arrows   = 0,
	Joystick = 1,
};

enum class VehiclePitchMode : uint8_t {
	Smart        = 0, // Aircraft + Bikes/BMX/Quad + Airborne stunt cars + Hydraulics/Turret
	Always       = 1, // Always show pitch/wheelie up/down buttons in Arrows mode
	AircraftOnly = 2, // Only show for Aircraft & Hydraulics/Turret
};

enum class PresetId : uint8_t {
	RoleplayHeavy    = 0, // Recommended default: RP + PUBG ergonomics
	PubgCompetitive  = 1, // 3/4-finger claw FPS layout
	TwoFingerMinimal = 2, // Clean 2-thumb layout
};

enum class CustomActionType : uint8_t {
	KeyCombo    = 0, // Presses all bits in keyMask simultaneously (with min pulse)
	ChatCommand = 1, // Sends command string (e.g. "/inv")
};

enum CustomContextMask : uint8_t {
	kCtxOnFoot    = 1u << 0,
	kCtxInVehicle = 1u << 1,
	kCtxBoth      = kCtxOnFoot | kCtxInVehicle,
};

struct ControlPlacement {
	float cx = 0.5f;      // Normalized screen center X [0.03 .. 0.97]
	float cy = 0.5f;      // Normalized screen center Y [0.05 .. 0.95]
	float scale = 1.0f;   // [0.50 .. 2.00]
	float opacity = 0.78f;// [0.15 .. 1.00]
	bool visible = true;
};

struct CustomButtonSlot {
	bool enabled = false;
	CustomActionType actionType = CustomActionType::KeyCombo;
	uint8_t contextMask = kCtxOnFoot;
	uint32_t keyMask = 0; // Bitmask of keyBit(SampKey::*)
	float cx = 0.50f;
	float cy = 0.72f;
	float scale = 0.85f;
	float opacity = 0.82f;
	char label[24] = {};
	char command[64] = {};

	void setLabel(std::string_view s)
	{
		const size_t n = std::min(s.size(), sizeof(label) - 1);
		if (n > 0) std::memcpy(label, s.data(), n);
		label[n] = '\0';
	}

	void setCommand(std::string_view s)
	{
		const size_t n = std::min(s.size(), sizeof(command) - 1);
		if (n > 0) std::memcpy(command, s.data(), n);
		command[n] = '\0';
	}
};

struct TouchHudPrefs {
	AimMode aimMode = AimMode::Toggle;
	JoystickMode joystickMode = JoystickMode::Floating;
	bool autoSprintLock = true;
	LeftFireMode leftFireMode = LeftFireMode::AimOnly;
	bool fireRightDragAim = true;
	VehicleSteeringMode vehicleSteering = VehicleSteeringMode::Joystick;
	VehiclePitchMode vehiclePitchMode = VehiclePitchMode::Smart;
	bool altSmartLatch = true;
	bool rpStripExpanded = true;
	bool showPassengerAlways = false;
	float globalOpacity = 0.82f; // [0.20 .. 1.00]
	float lookSensitivity = 1.0f;// [0.25 .. 3.00]
};

struct TouchHudLayout {
	static constexpr int kSchemaVersion = 3;

	PresetId activePreset = PresetId::RoleplayHeavy;
	TouchHudPrefs prefs{};
	std::array<ControlPlacement, kControlCount> controls{};
	std::array<CustomButtonSlot, kMaxCustomButtons> customButtons{};

	ControlPlacement& ctrl(ControlId id) { return controls[static_cast<size_t>(id)]; }
	const ControlPlacement& ctrl(ControlId id) const { return controls[static_cast<size_t>(id)]; }

	void clampAll()
	{
		prefs.globalOpacity = std::clamp(prefs.globalOpacity, 0.20f, 1.0f);
		prefs.lookSensitivity = std::clamp(prefs.lookSensitivity, 0.25f, 3.0f);
		for (auto& c : controls) {
			c.cx = std::clamp(c.cx, 0.03f, 0.97f);
			c.cy = std::clamp(c.cy, 0.05f, 0.95f);
			c.scale = std::clamp(c.scale, 0.50f, 2.00f);
			c.opacity = std::clamp(c.opacity, 0.15f, 1.00f);
		}
		for (auto& b : customButtons) {
			b.cx = std::clamp(b.cx, 0.03f, 0.97f);
			b.cy = std::clamp(b.cy, 0.05f, 0.95f);
			b.scale = std::clamp(b.scale, 0.50f, 2.00f);
			b.opacity = std::clamp(b.opacity, 0.15f, 1.00f);
			b.contextMask &= kCtxBoth;
			if (b.contextMask == 0) b.contextMask = kCtxOnFoot;
			b.label[sizeof(b.label) - 1] = '\0';
			b.command[sizeof(b.command) - 1] = '\0';
		}
	}
};

inline void applyCommonBasePlacements(TouchHudLayout& L)
{
	// On-foot PUBG tactical arc (keeps top-left Radar/Chat and top-right GTA Weapon HUD clear)
	L.ctrl(ControlId::Joystick)      = {0.14f, 0.74f, 1.00f, 0.78f, true};
	L.ctrl(ControlId::FireRight)     = {0.83f, 0.72f, 1.18f, 0.88f, true};
	L.ctrl(ControlId::FireLeft)      = {0.10f, 0.45f, 0.96f, 0.80f, true};
	L.ctrl(ControlId::Aim)           = {0.92f, 0.52f, 1.00f, 0.86f, true};
	L.ctrl(ControlId::Jump)          = {0.93f, 0.73f, 0.88f, 0.84f, true};
	L.ctrl(ControlId::Crouch)        = {0.85f, 0.88f, 0.84f, 0.84f, true};
	L.ctrl(ControlId::Sprint)        = {0.73f, 0.86f, 0.82f, 0.80f, true};
	L.ctrl(ControlId::InteractF)     = {0.72f, 0.67f, 0.86f, 0.86f, true};
	L.ctrl(ControlId::PassengerG)    = {0.64f, 0.67f, 0.78f, 0.84f, true};
	L.ctrl(ControlId::WeaponPrevQ)   = {0.78f, 0.34f, 0.78f, 0.80f, true};
	L.ctrl(ControlId::WeaponNextE)   = {0.845f, 0.34f, 0.78f, 0.80f, true};
	L.ctrl(ControlId::CamZoom)       = {0.92f, 0.34f, 0.78f, 0.80f, true};
	L.ctrl(ControlId::RpStrip)       = {0.31f, 0.91f, 0.95f, 0.84f, true};

	// In-vehicle controls (default left joystick + left side-look/yaw + right pedals/weapons)
	L.ctrl(ControlId::VehSteerLeft)   = {0.08f, 0.77f, 1.04f, 0.84f, true};
	L.ctrl(ControlId::VehSteerRight)  = {0.22f, 0.77f, 1.04f, 0.84f, true};
	L.ctrl(ControlId::VehJoystick)    = {0.15f, 0.76f, 1.00f, 0.78f, true};
	L.ctrl(ControlId::VehAccel)       = {0.91f, 0.72f, 1.15f, 0.88f, true};
	L.ctrl(ControlId::VehBrake)       = {0.80f, 0.78f, 1.00f, 0.86f, true};
	L.ctrl(ControlId::VehHandbrake)   = {0.91f, 0.52f, 0.92f, 0.86f, true};
	L.ctrl(ControlId::VehFire)        = {0.79f, 0.58f, 0.94f, 0.86f, true};
	L.ctrl(ControlId::VehHorn)        = {0.69f, 0.78f, 0.82f, 0.82f, true};
	// Place Q/E (Look Left/Right in cars, Horizontal Yaw in aircraft) on the LEFT side above steering
	// so the right thumb can simultaneously fire weapons / missiles / control gas & brake!
	L.ctrl(ControlId::VehLookLeftQ)   = {0.075f, 0.47f, 0.84f, 0.84f, true};
	L.ctrl(ControlId::VehLookRightE)  = {0.155f, 0.47f, 0.84f, 0.84f, true};
	L.ctrl(ControlId::VehExitF)       = {0.92f, 0.34f, 0.84f, 0.86f, true};
	L.ctrl(ControlId::VehSubMission2) = {0.69f, 0.58f, 0.78f, 0.82f, true};
	L.ctrl(ControlId::VehFlare)       = {0.69f, 0.44f, 0.78f, 0.86f, true};
	L.ctrl(ControlId::VehDriveBy)     = {0.91f, 0.52f, 0.96f, 0.88f, true};
	L.ctrl(ControlId::VehPitchUp)     = {0.15f, 0.63f, 0.84f, 0.82f, true};
	L.ctrl(ControlId::VehPitchDown)   = {0.15f, 0.90f, 0.84f, 0.82f, true};
	L.ctrl(ControlId::VehCamToggle)   = {0.835f, 0.34f, 0.80f, 0.80f, true};
}

inline TouchHudLayout makePresetLayout(PresetId preset)
{
	TouchHudLayout L{};
	L.activePreset = preset;
	applyCommonBasePlacements(L);

	for (auto& slot : L.customButtons) {
		slot = CustomButtonSlot{};
	}

	switch (preset) {
		case PresetId::PubgCompetitive:
			L.prefs.aimMode = AimMode::Toggle;
			L.prefs.joystickMode = JoystickMode::Floating;
			L.prefs.autoSprintLock = true;
			L.prefs.leftFireMode = LeftFireMode::Always;
			L.prefs.fireRightDragAim = true;
			L.prefs.vehicleSteering = VehicleSteeringMode::Joystick;
			L.prefs.altSmartLatch = true;
			L.prefs.rpStripExpanded = false;
			// Move left fire to classic claw upper-left index finger spot
			L.ctrl(ControlId::FireLeft) = {0.13f, 0.37f, 1.08f, 0.86f, true};
			L.ctrl(ControlId::Aim)      = {0.91f, 0.49f, 1.06f, 0.88f, true};
			L.ctrl(ControlId::Crouch)   = {0.84f, 0.88f, 0.90f, 0.86f, true};
			L.ctrl(ControlId::Jump)     = {0.93f, 0.71f, 0.92f, 0.86f, true};
			break;

		case PresetId::TwoFingerMinimal:
			L.prefs.aimMode = AimMode::Toggle;
			L.prefs.joystickMode = JoystickMode::Floating;
			L.prefs.autoSprintLock = true;
			L.prefs.leftFireMode = LeftFireMode::Hidden;
			L.prefs.fireRightDragAim = true;
			L.prefs.vehicleSteering = VehicleSteeringMode::Joystick;
			L.prefs.altSmartLatch = true;
			L.prefs.rpStripExpanded = false;
			L.ctrl(ControlId::FireLeft).visible = false;
			L.ctrl(ControlId::CamZoom).visible = false;
			break;

		case PresetId::RoleplayHeavy:
		default:
			L.prefs.aimMode = AimMode::Toggle;
			L.prefs.joystickMode = JoystickMode::Floating;
			L.prefs.autoSprintLock = true;
			L.prefs.leftFireMode = LeftFireMode::AimOnly;
			L.prefs.fireRightDragAim = true;
			L.prefs.vehicleSteering = VehicleSteeringMode::Joystick;
			L.prefs.altSmartLatch = true;
			L.prefs.rpStripExpanded = true;
			// Pre-configure 2 handy optional custom combo slots disabled by default
			// (players can enable or add their own in /hudedit)
			L.customButtons[0].enabled = false;
			L.customButtons[0].actionType = CustomActionType::KeyCombo;
			L.customButtons[0].contextMask = kCtxOnFoot;
			L.customButtons[0].keyMask = keyBit(SampKey::Walk) | keyBit(SampKey::Yes);
			L.customButtons[0].cx = 0.62f;
			L.customButtons[0].cy = 0.84f;
			L.customButtons[0].setLabel("ALT+Y");

			L.customButtons[1].enabled = false;
			L.customButtons[1].actionType = CustomActionType::KeyCombo;
			L.customButtons[1].contextMask = kCtxOnFoot;
			L.customButtons[1].keyMask = keyBit(SampKey::Walk) | keyBit(SampKey::No);
			L.customButtons[1].cx = 0.54f;
			L.customButtons[1].cy = 0.84f;
			L.customButtons[1].setLabel("ALT+N");
			break;
	}

	L.clampAll();
	return L;
}

// ---- Minimal self-contained JSON serialization (zero external deps, shared with host tests) ----

namespace detail {

inline void appendEscapedJsonString(std::string& out, const char* s)
{
	out.push_back('"');
	if (s) {
		for (; *s; ++s) {
			const unsigned char c = static_cast<unsigned char>(*s);
			if (c == '"' || c == '\\') {
				out.push_back('\\');
				out.push_back(static_cast<char>(c));
			}
			else if (c >= 0x20) {
				out.push_back(static_cast<char>(c));
			}
		}
	}
	out.push_back('"');
}

inline void appendFloat(std::string& out, float v)
{
	char buf[32];
	std::snprintf(buf, sizeof(buf), "%.4f", static_cast<double>(v));
	out += buf;
}

class JsonCursor {
public:
	explicit JsonCursor(std::string_view src) : m_src(src) {}

	void skipWs()
	{
		while (m_pos < m_src.size() && std::isspace(static_cast<unsigned char>(m_src[m_pos]))) {
			++m_pos;
		}
	}

	bool consume(char ch)
	{
		skipWs();
		if (m_pos < m_src.size() && m_src[m_pos] == ch) {
			++m_pos;
			return true;
		}
		return false;
	}

	char peek()
	{
		skipWs();
		return (m_pos < m_src.size()) ? m_src[m_pos] : '\0';
	}

	bool readString(std::string& out)
	{
		skipWs();
		if (m_pos >= m_src.size() || m_src[m_pos] != '"') return false;
		++m_pos;
		out.clear();
		while (m_pos < m_src.size()) {
			char c = m_src[m_pos++];
			if (c == '"') return true;
			if (c == '\\' && m_pos < m_src.size()) {
				char esc = m_src[m_pos++];
				switch (esc) {
					case '"':  out.push_back('"'); break;
					case '\\': out.push_back('\\'); break;
					case '/':  out.push_back('/'); break;
					case 'n':  out.push_back('\n'); break;
					case 't':  out.push_back('\t'); break;
					default:   out.push_back(esc); break;
				}
			}
			else {
				out.push_back(c);
			}
		}
		return false;
	}

	bool readBool(bool& out)
	{
		skipWs();
		if (m_src.substr(m_pos, 4) == "true") {
			m_pos += 4;
			out = true;
			return true;
		}
		if (m_src.substr(m_pos, 5) == "false") {
			m_pos += 5;
			out = false;
			return true;
		}
		return false;
	}

	bool readDouble(double& out)
	{
		skipWs();
		if (m_pos >= m_src.size()) return false;
		const size_t start = m_pos;
		if (m_src[m_pos] == '-') ++m_pos;
		bool anyDigit = false;
		while (m_pos < m_src.size() && std::isdigit(static_cast<unsigned char>(m_src[m_pos]))) {
			anyDigit = true;
			++m_pos;
		}
		if (m_pos < m_src.size() && m_src[m_pos] == '.') {
			++m_pos;
			while (m_pos < m_src.size() && std::isdigit(static_cast<unsigned char>(m_src[m_pos]))) {
				anyDigit = true;
				++m_pos;
			}
		}
		if (!anyDigit) {
			m_pos = start;
			return false;
		}
		std::string num(m_src.substr(start, m_pos - start));
		out = std::strtod(num.c_str(), nullptr);
		return std::isfinite(out);
	}

	bool readInt(int& out)
	{
		double d = 0.0;
		if (!readDouble(d)) return false;
		out = static_cast<int>(std::llround(d));
		return true;
	}

	bool readUint32(uint32_t& out)
	{
		double d = 0.0;
		if (!readDouble(d)) return false;
		if (d < 0.0) d = 0.0;
		out = static_cast<uint32_t>(std::llround(d));
		return true;
	}

	bool readFloat(float& out)
	{
		double d = 0.0;
		if (!readDouble(d)) return false;
		out = static_cast<float>(d);
		return true;
	}

	bool skipValue()
	{
		const char c = peek();
		if (c == '\0') return false;
		if (c == '"') {
			std::string dummy;
			return readString(dummy);
		}
		if (c == '{') {
			++m_pos;
			if (consume('}')) return true;
			while (true) {
				std::string k;
				if (!readString(k) || !consume(':') || !skipValue()) return false;
				if (consume('}')) return true;
				if (!consume(',')) return false;
			}
		}
		if (c == '[') {
			++m_pos;
			if (consume(']')) return true;
			while (true) {
				if (!skipValue()) return false;
				if (consume(']')) return true;
				if (!consume(',')) return false;
			}
		}
		if (c == 't' || c == 'f') {
			bool b = false;
			return readBool(b);
		}
		if (m_src.substr(m_pos, 4) == "null") {
			m_pos += 4;
			return true;
		}
		double d = 0.0;
		return readDouble(d);
	}

private:
	std::string_view m_src;
	size_t m_pos = 0;
};

inline bool parseControlPlacement(JsonCursor& cur, ControlPlacement& out)
{
	if (!cur.consume('{')) return false;
	if (cur.consume('}')) return true;
	while (true) {
		std::string key;
		if (!cur.readString(key) || !cur.consume(':')) return false;
		if (key == "x") { if (!cur.readFloat(out.cx)) return false; }
		else if (key == "y") { if (!cur.readFloat(out.cy)) return false; }
		else if (key == "scale") { if (!cur.readFloat(out.scale)) return false; }
		else if (key == "opacity") { if (!cur.readFloat(out.opacity)) return false; }
		else if (key == "visible") { if (!cur.readBool(out.visible)) return false; }
		else { if (!cur.skipValue()) return false; }
		if (cur.consume('}')) return true;
		if (!cur.consume(',')) return false;
	}
}

inline bool parseCustomButton(JsonCursor& cur, CustomButtonSlot& out)
{
	if (!cur.consume('{')) return false;
	if (cur.consume('}')) return true;
	while (true) {
		std::string key;
		if (!cur.readString(key) || !cur.consume(':')) return false;
		if (key == "enabled") { if (!cur.readBool(out.enabled)) return false; }
		else if (key == "type") {
			int v = 0;
			if (!cur.readInt(v)) return false;
			out.actionType = (v == 1) ? CustomActionType::ChatCommand : CustomActionType::KeyCombo;
		}
		else if (key == "ctx") {
			int v = 1;
			if (!cur.readInt(v)) return false;
			out.contextMask = static_cast<uint8_t>(v & kCtxBoth);
		}
		else if (key == "keys") { if (!cur.readUint32(out.keyMask)) return false; }
		else if (key == "x") { if (!cur.readFloat(out.cx)) return false; }
		else if (key == "y") { if (!cur.readFloat(out.cy)) return false; }
		else if (key == "scale") { if (!cur.readFloat(out.scale)) return false; }
		else if (key == "opacity") { if (!cur.readFloat(out.opacity)) return false; }
		else if (key == "label") {
			std::string s;
			if (!cur.readString(s)) return false;
			out.setLabel(s);
		}
		else if (key == "cmd") {
			std::string s;
			if (!cur.readString(s)) return false;
			out.setCommand(s);
		}
		else { if (!cur.skipValue()) return false; }
		if (cur.consume('}')) return true;
		if (!cur.consume(',')) return false;
	}
}

} // namespace detail

inline std::string serializeLayoutJson(const TouchHudLayout& L)
{
	std::string out;
	out.reserve(4096);
	out += "{\n";
	out += "  \"version\": " + std::to_string(TouchHudLayout::kSchemaVersion) + ",\n";
	out += "  \"preset\": " + std::to_string(static_cast<int>(L.activePreset)) + ",\n";
	out += "  \"prefs\": {\n";
	out += "    \"aim_mode\": " + std::to_string(static_cast<int>(L.prefs.aimMode)) + ",\n";
	out += "    \"joystick_mode\": " + std::to_string(static_cast<int>(L.prefs.joystickMode)) + ",\n";
	out += std::string("    \"auto_sprint_lock\": ") + (L.prefs.autoSprintLock ? "true" : "false") + ",\n";
	out += "    \"left_fire_mode\": " + std::to_string(static_cast<int>(L.prefs.leftFireMode)) + ",\n";
	out += std::string("    \"fire_right_drag_aim\": ") + (L.prefs.fireRightDragAim ? "true" : "false") + ",\n";
	out += "    \"vehicle_steering\": " + std::to_string(static_cast<int>(L.prefs.vehicleSteering)) + ",\n";
	out += "    \"vehicle_pitch_mode\": " + std::to_string(static_cast<int>(L.prefs.vehiclePitchMode)) + ",\n";
	out += std::string("    \"alt_smart_latch\": ") + (L.prefs.altSmartLatch ? "true" : "false") + ",\n";
	out += std::string("    \"rp_strip_expanded\": ") + (L.prefs.rpStripExpanded ? "true" : "false") + ",\n";
	out += std::string("    \"show_passenger_always\": ") + (L.prefs.showPassengerAlways ? "true" : "false") + ",\n";
	out += "    \"global_opacity\": "; detail::appendFloat(out, L.prefs.globalOpacity); out += ",\n";
	out += "    \"look_sensitivity\": "; detail::appendFloat(out, L.prefs.lookSensitivity); out += "\n";
	out += "  },\n";

	out += "  \"controls\": {\n";
	for (size_t i = 0; i < kControlCount; ++i) {
		const auto id = static_cast<ControlId>(i);
		const auto& c = L.controls[i];
		out += "    ";
		detail::appendEscapedJsonString(out, controlConfigKey(id));
		out += ": {\"x\": "; detail::appendFloat(out, c.cx);
		out += ", \"y\": "; detail::appendFloat(out, c.cy);
		out += ", \"scale\": "; detail::appendFloat(out, c.scale);
		out += ", \"opacity\": "; detail::appendFloat(out, c.opacity);
		out += ", \"visible\": "; out += (c.visible ? "true" : "false");
		out += "}";
		if (i + 1 < kControlCount) out += ",";
		out += "\n";
	}
	out += "  },\n";

	out += "  \"custom_buttons\": [\n";
	for (size_t i = 0; i < kMaxCustomButtons; ++i) {
		const auto& b = L.customButtons[i];
		out += "    {\"enabled\": "; out += (b.enabled ? "true" : "false");
		out += ", \"type\": " + std::to_string(static_cast<int>(b.actionType));
		out += ", \"ctx\": " + std::to_string(static_cast<int>(b.contextMask));
		out += ", \"keys\": " + std::to_string(b.keyMask);
		out += ", \"x\": "; detail::appendFloat(out, b.cx);
		out += ", \"y\": "; detail::appendFloat(out, b.cy);
		out += ", \"scale\": "; detail::appendFloat(out, b.scale);
		out += ", \"opacity\": "; detail::appendFloat(out, b.opacity);
		out += ", \"label\": "; detail::appendEscapedJsonString(out, b.label);
		out += ", \"cmd\": "; detail::appendEscapedJsonString(out, b.command);
		out += "}";
		if (i + 1 < kMaxCustomButtons) out += ",";
		out += "\n";
	}
	out += "  ]\n";
	out += "}\n";
	return out;
}

inline bool deserializeLayoutJson(std::string_view json, TouchHudLayout& out)
{
	TouchHudLayout parsed = makePresetLayout(PresetId::RoleplayHeavy);
	detail::JsonCursor cur(json);
	if (!cur.consume('{')) return false;
	if (cur.consume('}')) {
		out = parsed;
		return true;
	}

	int loadedVersion = 1;
	while (true) {
		std::string key;
		if (!cur.readString(key) || !cur.consume(':')) return false;

		if (key == "version") {
			int ver = 0;
			if (!cur.readInt(ver)) return false;
			loadedVersion = ver;
		}
		else if (key == "preset") {
			int p = 0;
			if (!cur.readInt(p)) return false;
			if (p >= 0 && p <= 2) {
				parsed.activePreset = static_cast<PresetId>(p);
			}
		}
		else if (key == "prefs") {
			if (!cur.consume('{')) return false;
			if (!cur.consume('}')) {
				while (true) {
					std::string pk;
					if (!cur.readString(pk) || !cur.consume(':')) return false;
					if (pk == "aim_mode") {
						int v = 0; if (!cur.readInt(v)) return false;
						parsed.prefs.aimMode = (v == 1) ? AimMode::Hold : AimMode::Toggle;
					}
					else if (pk == "joystick_mode") {
						int v = 0; if (!cur.readInt(v)) return false;
						parsed.prefs.joystickMode = (v == 1) ? JoystickMode::Fixed : JoystickMode::Floating;
					}
					else if (pk == "auto_sprint_lock") {
						if (!cur.readBool(parsed.prefs.autoSprintLock)) return false;
					}
					else if (pk == "left_fire_mode") {
						int v = 0; if (!cur.readInt(v)) return false;
						if (v == 0) parsed.prefs.leftFireMode = LeftFireMode::Always;
						else if (v == 1) parsed.prefs.leftFireMode = LeftFireMode::AimOnly;
						else parsed.prefs.leftFireMode = LeftFireMode::Hidden;
					}
					else if (pk == "fire_right_drag_aim") {
						if (!cur.readBool(parsed.prefs.fireRightDragAim)) return false;
					}
					else if (pk == "vehicle_steering") {
						int v = 0; if (!cur.readInt(v)) return false;
						parsed.prefs.vehicleSteering = (v == 1) ? VehicleSteeringMode::Joystick : VehicleSteeringMode::Arrows;
					}
					else if (pk == "vehicle_pitch_mode") {
						int v = 0; if (!cur.readInt(v)) return false;
						if (v == 1) parsed.prefs.vehiclePitchMode = VehiclePitchMode::Always;
						else if (v == 2) parsed.prefs.vehiclePitchMode = VehiclePitchMode::AircraftOnly;
						else parsed.prefs.vehiclePitchMode = VehiclePitchMode::Smart;
					}
					else if (pk == "alt_smart_latch") {
						if (!cur.readBool(parsed.prefs.altSmartLatch)) return false;
					}
					else if (pk == "rp_strip_expanded") {
						if (!cur.readBool(parsed.prefs.rpStripExpanded)) return false;
					}
					else if (pk == "show_passenger_always") {
						if (!cur.readBool(parsed.prefs.showPassengerAlways)) return false;
					}
					else if (pk == "global_opacity") {
						if (!cur.readFloat(parsed.prefs.globalOpacity)) return false;
					}
					else if (pk == "look_sensitivity") {
						if (!cur.readFloat(parsed.prefs.lookSensitivity)) return false;
					}
					else {
						if (!cur.skipValue()) return false;
					}
					if (cur.consume('}')) break;
					if (!cur.consume(',')) return false;
				}
			}
		}
		else if (key == "controls") {
			if (!cur.consume('{')) return false;
			if (!cur.consume('}')) {
				while (true) {
					std::string ck;
					if (!cur.readString(ck) || !cur.consume(':')) return false;
					bool matched = false;
					for (size_t i = 0; i < kControlCount; ++i) {
						const auto id = static_cast<ControlId>(i);
						if (ck == controlConfigKey(id)) {
							if (!detail::parseControlPlacement(cur, parsed.controls[i])) return false;
							matched = true;
							break;
						}
					}
					if (!matched && !cur.skipValue()) return false;
					if (cur.consume('}')) break;
					if (!cur.consume(',')) return false;
				}
			}
		}
		else if (key == "custom_buttons") {
			if (!cur.consume('[')) return false;
			size_t idx = 0;
			if (!cur.consume(']')) {
				while (true) {
					if (idx < kMaxCustomButtons) {
						if (!detail::parseCustomButton(cur, parsed.customButtons[idx])) return false;
					}
					else {
						if (!cur.skipValue()) return false;
					}
					++idx;
					if (cur.consume(']')) break;
					if (!cur.consume(',')) return false;
				}
			}
		}
		else {
			if (!cur.skipValue()) return false;
		}

		if (cur.consume('}')) break;
		if (!cur.consume(',')) return false;
	}

	// v1 -> v2 migration: if VehLookLeftQ / VehLookRightE were still at the old right-side default
	// (0.70, 0.34) / (0.76, 0.34), relocate them to the new left-side default above steering so
	// players can look/yaw left+right while firing on the right, without touching custom user layouts.
	if (loadedVersion < 2) {
		auto& ql = parsed.ctrl(ControlId::VehLookLeftQ);
		auto& qr = parsed.ctrl(ControlId::VehLookRightE);
		if (std::fabs(ql.cx - 0.70f) < 0.015f && std::fabs(ql.cy - 0.34f) < 0.015f) {
			ql.cx = 0.075f;
			ql.cy = 0.47f;
			ql.scale = 0.84f;
		}
		if (std::fabs(qr.cx - 0.76f) < 0.015f && std::fabs(qr.cy - 0.34f) < 0.015f) {
			qr.cx = 0.155f;
			qr.cy = 0.47f;
			qr.scale = 0.84f;
		}
	}

	parsed.clampAll();
	out = parsed;
	return true;
}

} // namespace touch_hud
