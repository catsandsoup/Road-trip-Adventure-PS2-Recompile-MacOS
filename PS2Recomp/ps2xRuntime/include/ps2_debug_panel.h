#ifndef PS2_DEBUG_PANEL_H
#define PS2_DEBUG_PANEL_H

#include <cstdlib>

class PS2Runtime;

class PS2DebugPanel
{
public:
    void initialize();
    void shutdown();
    void draw(PS2Runtime &runtime);

    bool isVisible() const { return m_visible; }
    void setVisible(bool visible) { m_visible = visible; }
    void toggleVisible() { m_visible = !m_visible; }

private:
    bool m_initialized = false;
    // Hidden by default (shipping UX); F1 toggles, PS2X_DEBUG_UI=1 starts with it open.
    bool m_visible = std::getenv("PS2X_DEBUG_UI") != nullptr;
    bool m_showRegisters = true;
    unsigned int m_memoryAddress = 0x00100000u;
    unsigned int m_memoryBytes = 0x100u;
};

#endif // PS2_DEBUG_PANEL_H
