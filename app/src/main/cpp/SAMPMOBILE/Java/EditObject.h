#ifndef SA_MP_MOBILE_EDITOBJECT_H
#define SA_MP_MOBILE_EDITOBJECT_H

#include <atomic>
#include <cstdint>

class ImGuiRenderer;

class EditObject {
public:
	enum eEditType {
		TYPE_NONE,
		TYPE_ATTACHED_OBJECT,
		TYPE_OBJECT
	};

	enum eResponse : uint32_t {
		EDIT_RESPONSE_CANCEL,
		EDIT_RESPONSE_FINAL,
		EDIT_RESPONSE_UPDATE
	};

	static inline int m_id = -1;
	static inline eEditType m_type = TYPE_NONE;
	static inline std::atomic<bool> m_visible{false};
	static inline bool m_playerObj = false;
	static inline uint32_t m_lastUpdate = 0;

public:
	static void startEditAttachedObject(int slot);
	static void startEditObject(uint16_t objectId);
	static void showGui(bool show);
	static void cancelEdit();
	static void exitEdit();

	// GameMain per-frame update (called from CGame_Process hook)
	static void processPending();

	// Native ImGui + 3D Gizmo render pass (called from UI::drawList on GameMain)
	static void render(ImGuiRenderer* renderer);

	// Multi-touch routing & walk-mode state (called from InputArbiter / TouchHud)
	static bool isWalkMode();
	static bool isUiPanelHitSnapshot(float x, float y);
	static bool handleTouch(int type, int pointerId, float x, float y);
	static void onTouchCancel();

private:
	static void applyReset();
	static void applyExit();
	static void applySave();
};

#endif // SA_MP_MOBILE_EDITOBJECT_H
