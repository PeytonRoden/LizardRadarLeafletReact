



//put it at the top of the map for now, floating dropdown menu



function formatTiltTime(utcTime) {
    const match = utcTime?.match(/^(\d{4})-(\d{2})-(\d{2}) (\d{2})-(\d{2})-(\d{2})$/);
    if (!match) return utcTime || "Unknown time";

    const [, year, month, day, hour, minute, second] = match;
    const date = new Date(Date.UTC(year, month - 1, day, hour, minute, second));
    const parts = new Intl.DateTimeFormat(undefined, {
        hour: "numeric",
        minute: "2-digit",
        second: "2-digit",
        day: "numeric",
        month: "numeric",
        year: "numeric",
        hour12: true,
    }).formatToParts(date).reduce((values, part) => {
        values[part.type] = part.value;
        return values;
    }, {});

    return `${parts.hour}:${parts.minute}:${parts.second} ${parts.day}/${parts.month}/${parts.year} ${parts.dayPeriod}`;
}

export default function TiltAngleSelection({ selectedTiltAngle, setSelectedTiltAngle, tiltAngles, tiltInfo = [] }) {
    const options = tiltInfo.length > 0
        ? tiltInfo
        : tiltAngles.map((angle) => ({ angle, time: null }));
    const selectedInfo = options.find(({ angle }) => Math.abs(angle - selectedTiltAngle) < 0.0001);

    return (
        <div className="selection-card">
            <div className="selection-card__label">
                Tilt Angle
            </div>
            <select
                value={selectedTiltAngle}
                onChange={(e) => setSelectedTiltAngle(Number(e.target.value))}
                className="selection-card__select"
            >
                {options.map(({ angle, time }) => (
                    <option key={angle} value={angle}>
                        {angle.toFixed(4)}°{time ? ` - ${formatTiltTime(time)}` : ""}
                    </option>
                ))}
            </select>
            {selectedInfo?.time && (
                <div className="selection-card__hint">
                    {selectedInfo.angle.toFixed(4)}° — {formatTiltTime(selectedInfo.time)}
                </div>
            )}
        </div>
    );
}