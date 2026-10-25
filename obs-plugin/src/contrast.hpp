#pragma once

#include <QApplication>
#include <QColor>
#include <QPalette>
#include <QString>

#include <algorithm>
#include <cmath>

namespace livedelay {

inline double ChannelLuminance(int channel)
{
	const double s = channel / 255.0;
	return s <= 0.03928 ? s / 12.92 : std::pow((s + 0.055) / 1.055, 2.4);
}

inline double Luminance(const QColor &c)
{
	return 0.2126 * ChannelLuminance(c.red()) + 0.7152 * ChannelLuminance(c.green()) +
	       0.0722 * ChannelLuminance(c.blue());
}

inline double ContrastRatio(const QColor &fg, const QColor &bg)
{
	const double a = Luminance(fg);
	const double b = Luminance(bg);
	const double hi = std::max(a, b);
	const double lo = std::min(a, b);
	return (hi + 0.05) / (lo + 0.05);
}

// OBS paints WindowText for the theme, and leaves PlaceholderText at Qt's
// light-theme default, which is near black. Hints that used that role
// disappeared on the dark theme.
inline QColor OnTheme(bool muted = false)
{
	const QPalette pal = QApplication::palette();
	const QColor bg = pal.color(QPalette::Window);
	QColor fg = pal.color(QPalette::WindowText);
	if (ContrastRatio(fg, bg) < 4.5)
		fg = Luminance(bg) > 0.4 ? QColor(26, 26, 26) : QColor(232, 232, 232);
	if (!muted)
		return fg;
	QColor soft = fg;
	for (int i = 0; i < 5; ++i) {
		const QColor next(int(fg.red() * 0.72 + bg.red() * 0.28), int(fg.green() * 0.72 + bg.green() * 0.28),
				  int(fg.blue() * 0.72 + bg.blue() * 0.28));
		if (ContrastRatio(next, bg) < 4.5)
			break;
		soft = next;
		fg = next;
	}
	return soft;
}

inline QString ThemeColor(bool muted = false)
{
	return OnTheme(muted).name();
}

// QLabel rich text ignores the widget color and defaults to black.
inline QString Rich(const QString &html, bool muted = false)
{
	return QStringLiteral("<span style=\"color:%1;\">%2</span>").arg(ThemeColor(muted), html);
}

} // namespace livedelay
