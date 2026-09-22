



//put it at the top of the map for now, floating dropdown menu



export function formatTiltTime(utcTime) {
    const match = utcTime?.match(/^(\d{4})-(\d{2})-(\d{2}) (\d{2})-(\d{2})-(\d{2})$/);
    if (!match) return { date: "", time: utcTime || "Unknown time" };

    const [, year, month, day, hour, minute, second] = match;
    const value = new Date(Date.UTC(year, month - 1, day, hour, minute, second));

    return {
        date: new Intl.DateTimeFormat(undefined, {
            day: "numeric",
            month: "short",
            year: "numeric",
        }).format(value),
        time: new Intl.DateTimeFormat(undefined, {
            hour: "numeric",
            minute: "2-digit",
            second: "2-digit",
        }).format(value),
    };
}

export default function TiltAngleSelection({ selectedTiltAngle, setSelectedTiltAngle, tiltAngles, tiltInfo = [] }) {
    const options = tiltInfo.length > 0
        ? tiltInfo
        : tiltAngles.map((angle) => ({ angle, time: null }));
    const selectedInfo = options.find(({ angle }) => Math.abs(angle - selectedTiltAngle) < 0.0001);
    const selectedTime = selectedInfo?.time ? formatTiltTime(selectedInfo.time) : null;

    return (
        <div className="selection-card selection-card--tilt">
            <div className="selection-card__label">
                Tilt angle
            </div>
            <select
                value={selectedTiltAngle}
                onChange={(e) => setSelectedTiltAngle(Number(e.target.value))}
                className="selection-card__select"
            >
                {options.map(({ angle, time }) => {
                    const timestamp = time ? formatTiltTime(time) : null;

                    return (
                        <option key={angle} value={angle}>
                            {angle.toFixed(4)}°{timestamp ? ` — ${timestamp.time} · ${timestamp.date}` : ""}
                        </option>
                    );
                })}
            </select>
            {selectedTime && (
                <div className="tilt-time">
                    <span className="tilt-time__label">Scan time</span>
                    <span className="tilt-time__value">{selectedTime.time}</span>
                    <span className="tilt-time__date">{selectedTime.date}</span>
                </div>
            )}
        </div>
    );
}