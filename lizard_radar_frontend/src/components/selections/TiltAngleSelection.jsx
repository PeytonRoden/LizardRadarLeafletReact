



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

export default function TiltAngleSelection({ selectedTiltIndex, setSelectedTiltIndex, tiltAngles, tiltInfo = [] }) {
    const options = tiltInfo.length > 0
        ? tiltInfo
        : tiltAngles.map((angle) => ({ angle, time: null }));
    // Sort display order by scan time, but keep the original index as the
    // option value since selectedTiltIndex maps to the WASM tilt index.
    const sortedOptions = options
        .map((option, index) => ({ option, index }))
        .sort((a, b) => {
            if (a.option.time && b.option.time) return a.option.time < b.option.time ? -1 : a.option.time > b.option.time ? 1 : 0;
            if (a.option.time) return -1;
            if (b.option.time) return 1;
            return a.index - b.index;
        });
    const selectedIndex = options.length > 0
        ? Math.min(Math.max(selectedTiltIndex, 0), options.length - 1)
        : 0;
    const selectedInfo = options[selectedIndex];
    const selectedTime = selectedInfo?.time ? formatTiltTime(selectedInfo.time) : null;

    return (
        <div className="selection-card selection-card--tilt">
            <div className="selection-card__label">
                Tilt angle
            </div>
            <select
                value={selectedIndex}
                onChange={(e) => setSelectedTiltIndex(Number(e.target.value))}
                className="selection-card__select"
            >
                {sortedOptions.map(({ option: { angle, time }, index }) => {
                    const timestamp = time ? formatTiltTime(time) : null;

                    return (
                        <option key={`${angle}-${time}-${index}`} value={index}>
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