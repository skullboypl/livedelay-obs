#pragma once

#include "contrast.hpp"
#include "settings.hpp"

#include <QApplication>
#include <QColor>
#include <QPalette>
#include <QString>

namespace livedelay {

// The colours of the product: web/public/styles.css :root, the same ones the
// site and the standalone app use. Accents are named after what they mean.
struct Palette {
	bool painted; // true = the dock draws its own surface instead of OBS's
	QColor surface; // panel background
	QColor raise; // cards inside the panel
	QColor line; // hairlines
	QColor text;
	QColor muted;
	QColor tally; // the delay is on air
	QColor live; // viewers see the live edge
	QColor arm; // buffer armed, and warnings
};

inline Palette ThemePalette(DockTheme theme)
{
	switch (theme) {
	case DockTheme::Dark:
		return {true,	       QColor("#070707"), QColor("#111113"), QColor("#1e1e21"), QColor("#ebe8e3"),
			QColor("#8a8780"), QColor("#ff3b2f"), QColor("#3ddc84"), QColor("#ffbf3c")};
	case DockTheme::Contrast:
		// Same hues pushed until every one clears 4.5:1 on pure black.
		return {true,	       QColor("#000000"), QColor("#101010"), QColor("#6b6b6b"), QColor("#ffffff"),
			QColor("#d8d5ce"), QColor("#ff6f63"), QColor("#5cff9d"), QColor("#ffd166")};
	case DockTheme::Obs:
	default:
		break;
	}
	// Follow OBS: its surface, its text, our accents.
	const QPalette pal = QApplication::palette();
	return {false,
		pal.color(QPalette::Window),
		pal.color(QPalette::Base),
		pal.color(QPalette::Mid),
		OnTheme(false),
		OnTheme(true),
		QColor("#ff3b2f"),
		QColor("#3ddc84"),
		QColor("#ffbf3c")};
}

// Black or white, whichever stays readable on `bg`.
inline QColor InkOn(const QColor &bg)
{
	return Luminance(bg) > 0.4 ? QColor("#070707") : QColor("#ffffff");
}

inline QString Rgba(const QColor &c, double alpha)
{
	return QStringLiteral("rgba(%1,%2,%3,%4)").arg(c.red()).arg(c.green()).arg(c.blue()).arg(alpha, 0, 'f', 2);
}

// One action is full and loud — the one the current state is waiting for.
// Everything else stays outlined and quiet, on either OBS theme.
inline QString FilledButton(const QColor &accent)
{
	const QString ink = InkOn(accent).name();
	return QStringLiteral("QPushButton { background: %1; border: 1px solid %1; color: %2; border-radius: 6px;"
			      " padding: 4px 10px; font-weight: 700; }"
			      "QPushButton:hover { background: %3; border-color: %3; }"
			      "QPushButton:pressed { background: %4; border-color: %4; }"
			      "QPushButton:disabled { background: transparent; border-color: %5; color: %6; }")
		.arg(accent.name(), ink, accent.lighter(112).name(), accent.darker(115).name(), Rgba(accent, .35),
		     Rgba(accent, .55));
}

inline QString GhostButton(const QColor &accent, const QColor &text)
{
	return QStringLiteral("QPushButton { background: %1; border: 1px solid %2; color: %3; border-radius: 6px;"
			      " padding: 4px 10px; font-weight: 600; }"
			      "QPushButton:hover { background: %4; }"
			      "QPushButton:pressed { background: %5; }"
			      "QPushButton:disabled { background: transparent; border-color: %6; color: %7; }")
		.arg(Rgba(accent, .14), Rgba(accent, .45), text.name(), Rgba(accent, .26), Rgba(accent, .36),
		     Rgba(accent, .22), Rgba(text, .45));
}

// Buttons with no accent of their own: invisible until needed.
inline QString QuietButton(const Palette &p)
{
	return QStringLiteral("QPushButton { background: transparent; border: 1px solid %1; color: %2;"
			      " border-radius: 6px; padding: 4px 10px; }"
			      "QPushButton:hover { background: %3; border-color: %4; }"
			      "QPushButton:pressed { background: %5; }"
			      "QPushButton:disabled { color: %6; border-color: %7; }")
		.arg(Rgba(p.line, .9), p.text.name(), Rgba(p.text, .08), Rgba(p.text, .35), Rgba(p.text, .14),
		     Rgba(p.text, .40), Rgba(p.line, .5));
}

// Only the painted themes take the whole panel over; under "same as OBS" the
// dock keeps OBS's own surface and only the accents below are ours.
inline QString DockSheet(const Palette &p)
{
	if (!p.painted)
		return QString();
	return QStringLiteral("QWidget#ldDock { background: %1; }"
			      "QWidget#ldDock QLabel { color: %2; background: transparent; }"
			      "QWidget#ldDock QSpinBox { background: %3; color: %2; border: 1px solid %4;"
			      " border-radius: 6px; padding: 2px 6px; selection-background-color: %5; }"
			      "QWidget#ldDock QProgressBar { background: %4; border: none; border-radius: 3px; }"
			      "QWidget#ldDock QProgressBar::chunk { background: %6; border-radius: 3px; }"
			      "QWidget#ldDock QToolTip { background: %3; color: %2; border: 1px solid %4; }")
		.arg(p.surface.name(), p.text.name(), p.raise.name(), p.line.name(), Rgba(p.tally, .55), p.arm.name());
}

// A tinted strip: the banners, and the frame around the platform rows.
inline QString CardSheet(const char *id, const Palette &p, const QColor &accent, bool tinted = true)
{
	return QStringLiteral("#%1 { background: %2; border: 1px solid %3; border-radius: 6px; }")
		.arg(QString::fromLatin1(id), tinted ? Rgba(accent, p.painted ? .18 : .14) : QStringLiteral("transparent"),
		     Rgba(accent, tinted ? .45 : .9));
}

} // namespace livedelay
