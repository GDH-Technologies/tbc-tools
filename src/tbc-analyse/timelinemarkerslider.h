#ifndef TIMELINEMARKERSLIDER_H
#define TIMELINEMARKERSLIDER_H

#include <QColor>
#include <QSlider>
#include <QStringList>
#include <QVector>
#include <QtGlobal>

// A span painted over the groove (slider positions, inclusive).
struct TimelineSegmentSpan {
    qint32 startPosition = -1;
    qint32 endPosition = -1;
    QColor color;
};

class TimelineMarkerSlider : public QSlider
{
    Q_OBJECT

public:
    explicit TimelineMarkerSlider(QWidget *parent = nullptr);
    void setMarkerFrames(qint32 inFrame, qint32 outFrame, const QVector<qint32> &noteFrames);
    // Recording-segment boundaries (one tick each, with a tooltip) and the
    // spans that deserve a tint (noise, blank, disabled clips).
    void setSegmentMarkers(const QVector<qint32> &boundaryPositions,
                           const QStringList &boundaryTooltips,
                           const QVector<TimelineSegmentSpan> &spans);

protected:
    void paintEvent(QPaintEvent *event) override;
    bool event(QEvent *event) override;

private:
    int xForPosition(qint32 position, const QRect &grooveRect, bool upsideDown) const;
    qint32 inFrame_ = -1;
    qint32 outFrame_ = -1;
    QVector<qint32> noteFrames_;
    QVector<qint32> segmentBoundaries_;
    QStringList segmentTooltips_;
    QVector<TimelineSegmentSpan> segmentSpans_;
};

#endif // TIMELINEMARKERSLIDER_H
