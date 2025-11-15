#include "VUMeter.h"

// Define static members for synchronized blinking rhythm
bool VUMeter::s_blinkState = false;
QTimer* VUMeter::s_globalBlinkTimer = nullptr;
std::vector<VUMeter*> VUMeter::s_allMeters;
int VUMeter::s_activeBlinkingMeters = 0;
