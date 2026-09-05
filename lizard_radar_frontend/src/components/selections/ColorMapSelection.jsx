import { useEffect, useMemo } from 'react';
import { colorMaps } from '../../utils/colorMaps';

export default function ColorMapSelection({ selectedMoment, selectedColorMap, setSelectedColorMap }) {
    const availableColorMaps = useMemo(
        () => Object.entries(colorMaps[selectedMoment] ?? {}),
        [selectedMoment]
    );
    const firstColorMap = availableColorMaps[0]?.[0];
    const selectedMomentColorMap = selectedColorMap?.split('/')[0] === selectedMoment
        ? selectedColorMap
        : null;

    useEffect(() => {
        if (firstColorMap && !selectedMomentColorMap) {
            setSelectedColorMap(`${selectedMoment}/${firstColorMap}`);
        }
    }, [firstColorMap, selectedMoment, selectedMomentColorMap, setSelectedColorMap]);
    return (
        <div className="selection-card">
            <div className="selection-card__label">
                Color Map
            </div>
            <select
                value={selectedMomentColorMap ?? ''}
                onChange={(e) => setSelectedColorMap(e.target.value)}
                className="selection-card__select"
                disabled={availableColorMaps.length === 0}
            >
                {availableColorMaps.map(([name]) => (
                    <option key={name} value={`${selectedMoment}/${name}`}>
                        {name}
                    </option>
                ))}
            </select>
        </div>
    );
}