#include "timelinemarkerslider.h"

#include <QHelpEvent>
#include <QPainter>
#include <QStyle>
#include <QStyleOptionSlider>
#include <QToolTip>

TimelineMarkerSlider::TimelineMarkerSlider(QWidget *parent)
    : QSlider(parent)
{
}

void TimelineMarkerSlider::setMarkerFrames(qint32 inFrame, qint32 outFrame, const QVector<qint32> &noteFrames)
{
    if (inFrame_ == inFrame && outFrame_ == outFrame && noteFrames_ == noteFrames) {
        return;
    }

    inFrame_ = inFrame;
    outFrame_ = outFrame;
    noteFrames_ = noteFrames;
    update();
}

void TimelineMarkerSlider::setSegmentMarkers(const QVector<qint32> &boundaryPositions,
                                             const QStringList &boundaryTooltips,
                                             const QVector<TimelineSegmentSpan> &spans)
{
    bool spansEqual = spans.size() == segmentSpans_.size();
    for (int i = 0; spansEqual && i < spans.size(); ++i) {
        spansEqual = spans.at(i).startPosition == segmentSpans_.at(i).startPosition
                     && spans.at(i).endPosition == segmentSpans_.at(i).endPosition
                     && spans.at(i).color == segmentSpans_.at(i).color;
    }
    if (spansEqual && segmentBoundaries_ == boundaryPositions && segmentTooltips_ == boundaryTooltips) {
        return;
    }
    segmentBoundaries_ = boundaryPositions;
    segmentTooltips_ = boundaryTooltips;
    segmentSpans_ = spans;
    update();
}

int TimelineMarkerSlider::xForPosition(qint32 position, const QRect &grooveRect, bool upsideDown) const
{
    const int trackLength = grooveRect.width() - 1;
    const qint32 clamped = qBound<qint32>(minimum(), position, maximum());
    return grooveRect.left()
           + QStyle::sliderPositionFromValue(minimum(), maximum(), clamped, trackLength, upsideDown);
}

void TimelineMarkerSlider::paintEvent(QPaintEvent *event)
{
    QSlider::paintEvent(event);

    if (orientation() != Qt::Horizontal || maximum() <= minimum()) {
        return;
    }

    QStyleOptionSlider option;
    initStyleOption(&option);
    const QRect grooveRect = style()->subControlRect(QStyle::CC_Slider, &option, QStyle::SC_SliderGroove, this);
    if (!grooveRect.isValid() || grooveRect.width() <= 1) {
        return;
    }

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, false);

    const int lineTop = grooveRect.top() - 3;
    const int lineBottom = grooveRect.bottom() + 3;

    // Segment spans first, tinting the groove
    for (const TimelineSegmentSpan &span : segmentSpans_) {
        if (span.endPosition < span.startPosition || span.endPosition < minimum() || span.startPosition > maximum()) {
            continue;
        }
        const int x1 = xForPosition(span.startPosition, grooveRect, option.upsideDown);
        const int x2 = xForPosition(span.endPosition, grooveRect, option.upsideDown);
        const QRect spanRect(qMin(x1, x2), grooveRect.top(), qAbs(x2 - x1) + 1, grooveRect.height());
        painter.fillRect(spanRect, span.color);
    }

    // Segment boundary ticks, a little taller than note ticks
    {
        QPen pen(QColor(255, 170, 0));
        pen.setWidth(2);
        painter.setPen(pen);
        for (qint32 position : segmentBoundaries_) {
            if (position < minimum() || position > maximum()) {
                continue;
            }
            const int x = xForPosition(position, grooveRect, option.upsideDown);
            painter.drawLine(x, lineTop - 2, x, lineBottom + 2);
        }
    }

    auto drawMarkerLine = [&](qint32 framePosition, const QColor &color) {
        if (framePosition < minimum() || framePosition > maximum()) {
            return;
        }
        const int x = xForPosition(framePosition, grooveRect, option.upsideDown);
        QPen pen(color);
        pen.setWidth(2);
        painter.setPen(pen);
        painter.drawLine(x, lineTop, x, lineBottom);
    };

    drawMarkerLine(inFrame_, QColor(24, 190, 24));
    drawMarkerLine(outFrame_, QColor(220, 45, 45));
    for (qint32 framePosition : noteFrames_) {
        drawMarkerLine(framePosition, QColor(30, 120, 255));
    }
}

bool TimelineMarkerSlider::event(QEvent *event)
{
    if (event && event->type() == QEvent::ToolTip && !segmentBoundaries_.isEmpty()
        && orientation() == Qt::Horizontal && maximum() > minimum()) {
        auto *helpEvent = static_cast<QHelpEvent *>(event);
        QStyleOptionSlider option;
        initStyleOption(&option);
        const QRect grooveRect = style()->subControlRect(QStyle::CC_Slider, &option, QStyle::SC_SliderGroove, this);
        if (grooveRect.isValid() && grooveRect.width() > 1) {
            int bestDistance = 6;
            int bestIndex = -1;
            for (int i = 0; i < segmentBoundaries_.size(); ++i) {
                const int x = xForPosition(segmentBoundaries_.at(i), grooveRect, option.upsideDown);
                const int distance = qAbs(x - helpEvent->pos().x());
                if (distance < bestDistance) {
                    bestDistance = distance;
                    bestIndex = i;
                }
            }
            if (bestIndex >= 0 && bestIndex < segmentTooltips_.size()) {
                QToolTip::showText(helpEvent->globalPos(), segmentTooltips_.at(bestIndex), this);
                return true;
            }
        }
        QToolTip::hideText();
        event->ignore();
        return true;
    }
    return QSlider::event(event);
}
