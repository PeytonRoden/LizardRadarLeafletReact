
//put it at the top of the map for now, floating dropdown menu




export default function MomentSelection({ selectedMoment, setSelectedMoment }) {
    return (
        <div className="selection-card">
            <div className="selection-card__label">
                Moment
            </div>
            <select
                value={selectedMoment}
                onChange={(e) => setSelectedMoment(e.target.value)}
                className="selection-card__select"
            >
                <option value="REF">Reflectivity</option>
                <option value="VEL">Velocity</option>
                <option value="SW">Spectrum Width</option>
                <option value="ZDR">Differential Reflectivity</option>
                <option value="RHO">Correlation Coefficient</option>
                <option value="PHI">Differential Phase</option>
            </select>
        </div>
    );
}