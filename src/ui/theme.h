// ProdMesh theme tokens. Mirrors ProdMesh RTA's `theme` namespace and
// APP_STYLE (prodmesh-rta/src/widgets.h, main.cpp) so the products look like
// one family; extract into a shared library once a third product needs it.
#pragma once

#include <QColor>

namespace pm::ui::theme {

// From RTA.
inline const QColor bg("#14161c");
inline const QColor panel("#1a1d24");
inline const QColor grid("#2a2e39");
inline const QColor border("#323848");
inline const QColor borderHi("#46506a");
inline const QColor text("#8a92a6");
inline const QColor textHi("#c8cede");
inline const QColor textMax("#e8ecf4");
inline const QColor accent("#2fbf9b");    // RTA bar
inline const QColor accentHi("#5ce0bd");  // RTA barTop
inline const QColor warn("#e0c05c");      // RTA peak
inline const QColor faint("#3a6b5f");

// Mix additions. Colour only ever means something: signal, hot, clip,
// muted, on air.
inline const QColor clip("#e5484d");
inline const QColor mute("#c2393e");
inline const QColor control("#232733");

inline constexpr const char* kAppStyle = R"(
QWidget { background: #1a1d24; color: #c8cede; }
QLabel { background: transparent; }
QScrollArea { border: none; }
QScrollBar:horizontal { height: 10px; background: #14161c; }
QScrollBar::handle:horizontal { background: #323848; border-radius: 4px; min-width: 40px; }
QScrollBar::add-line, QScrollBar::sub-line { width: 0; }
QToolTip { background: #232733; color: #e8ecf4; border: 1px solid #46506a; }
)";

}  // namespace pm::ui::theme
