



//put it at the top of the map for now, floating dropdown menu



export default function DataTimeSelection({ selectedDataTime, setSelectedDataTime }) {
    return (
        <div className="selection-card">
            <div className="selection-card__label">
                Data Time
            </div>
            <select
                value={selectedDataTime}
                onChange={(e) => setSelectedDataTime(e.target.value)}
                className="selection-card__select"
            >
                <option value="Latest">Latest</option>
                <option value="Looped_Latest">Looped Latest</option>
                <option value="Historical">Historical</option>
                <option value="Looped_Historical">Looped Historical</option>
            </select>
        </div>
    );
}