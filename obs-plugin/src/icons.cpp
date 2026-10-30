#include "icons.hpp"

#include <QPainter>
#include <QPainterPath>
#include <QPixmap>

#include <cmath>

namespace livedelay {

namespace {

// All icons are drawn on a 24x24 grid.
void Draw(QPainter &p, Icon kind, const QColor &c)
{
	QPen pen(c, 2.2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
	p.setPen(pen);
	p.setBrush(Qt::NoBrush);
	switch (kind) {
	case Icon::Arm: // ring with a filled centre: "recording into the buffer"
		p.drawEllipse(QPointF(12, 12), 8.5, 8.5);
		p.setBrush(c);
		p.setPen(Qt::NoPen);
		p.drawEllipse(QPointF(12, 12), 3.6, 3.6);
		break;
	case Icon::Activate: // clock
		p.drawEllipse(QPointF(12, 12), 8.5, 8.5);
		p.drawLine(QPointF(12, 12), QPointF(12, 7));
		p.drawLine(QPointF(12, 12), QPointF(15.5, 14));
		break;
	case Icon::Live: // skip to the live edge
	case Icon::LiveAfter: {
		QPainterPath tri;
		tri.moveTo(5, 6);
		tri.lineTo(14, 12);
		tri.lineTo(5, 18);
		tri.closeSubpath();
		p.setBrush(c);
		p.drawPath(tri);
		p.drawLine(QPointF(18, 6), QPointF(18, 18));
		if (kind == Icon::LiveAfter) { // small clock badge
			p.setBrush(Qt::NoBrush);
			p.setPen(QPen(c, 1.6, Qt::SolidLine, Qt::RoundCap));
			p.drawEllipse(QPointF(19, 19), 4, 4);
			p.drawLine(QPointF(19, 19), QPointF(19, 16.8));
		}
		break;
	}
	case Icon::Disarm: // circle with a cross
		p.drawEllipse(QPointF(12, 12), 8.5, 8.5);
		p.drawLine(QPointF(9, 9), QPointF(15, 15));
		p.drawLine(QPointF(15, 9), QPointF(9, 15));
		break;
	case Icon::Settings: // sliders
		for (int i = 0; i < 3; i++) {
			const double y = 6 + i * 6, knob = i == 1 ? 15 : 9;
			p.drawLine(QPointF(4, y), QPointF(20, y));
			p.setBrush(c);
			p.drawEllipse(QPointF(knob, y), 2.4, 2.4);
			p.setBrush(Qt::NoBrush);
		}
		break;
	case Icon::Preview: { // screen with a play mark
		p.drawRoundedRect(QRectF(3, 5, 18, 12), 2, 2);
		p.drawLine(QPointF(8, 20), QPointF(16, 20));
		QPainterPath tri;
		tri.moveTo(10.5, 8.5);
		tri.lineTo(14.5, 11);
		tri.lineTo(10.5, 13.5);
		tri.closeSubpath();
		p.setBrush(c);
		p.setPen(Qt::NoPen);
		p.drawPath(tri);
		break;
	}
	case Icon::Panel: { // browser window with a slider on its page
		p.drawRoundedRect(QRectF(3, 4.5, 18, 15), 2, 2);
		p.drawLine(QPointF(3, 9), QPointF(21, 9));
		p.drawLine(QPointF(7, 13), QPointF(17, 13));
		p.drawLine(QPointF(7, 16.5), QPointF(17, 16.5));
		p.setBrush(c);
		p.setPen(Qt::NoPen);
		p.drawEllipse(QPointF(14, 13), 1.9, 1.9);
		p.drawEllipse(QPointF(9.5, 16.5), 1.9, 1.9);
		break;
	}
	case Icon::Help: {
		p.drawEllipse(QPointF(12, 12), 8.5, 8.5);
		QPainterPath q;
		q.moveTo(9.5, 9.6);
		q.cubicTo(9.5, 6.8, 14.8, 6.8, 14.8, 9.8);
		q.cubicTo(14.8, 11.6, 12, 11.8, 12, 13.8);
		p.drawPath(q);
		p.setBrush(c);
		p.setPen(Qt::NoPen);
		p.drawEllipse(QPointF(12, 17), 1.3, 1.3);
		break;
	}
	case Icon::Restart: {
		p.drawArc(QRectF(4, 4, 16, 16), 60 * 16, 290 * 16);
		QPainterPath head;
		head.moveTo(17.5, 3.5);
		head.lineTo(17.5, 8.5);
		head.lineTo(12.5, 8.5);
		p.drawPath(head);
		break;
	}
	case Icon::Play: {
		QPainterPath tri;
		tri.moveTo(7, 5);
		tri.lineTo(18, 12);
		tri.lineTo(7, 19);
		tri.closeSubpath();
		p.setBrush(c);
		p.drawPath(tri);
		break;
	}
	case Icon::Stop:
		p.setBrush(c);
		p.drawRoundedRect(QRectF(6.5, 6.5, 11, 11), 2, 2);
		break;
	}
}

} // namespace

QIcon MakeIcon(Icon kind, const QColor &color)
{
	QIcon icon;
	for (int size : {16, 24, 32, 48}) {
		QPixmap pm(size, size);
		pm.fill(Qt::transparent);
		QPainter p(&pm);
		p.setRenderHint(QPainter::Antialiasing);
		p.scale(size / 24.0, size / 24.0);
		Draw(p, kind, color);
		p.end();
		icon.addPixmap(pm, QIcon::Normal);
		// Disabled buttons get the same shape in a muted grey.
		QPixmap off(size, size);
		off.fill(Qt::transparent);
		QPainter q(&off);
		q.setRenderHint(QPainter::Antialiasing);
		q.scale(size / 24.0, size / 24.0);
		Draw(q, kind, QColor(128, 132, 142, 150));
		q.end();
		icon.addPixmap(off, QIcon::Disabled);
	}
	return icon;
}

} // namespace livedelay
