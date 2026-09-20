import { useEffect, useState } from "react";
import { getAvailableDays, getAvailableMonths, getAvailableTimes, getAvailableYears } from "../../api/nexradApi";


//put it at the top of the map for now, floating dropdown menu


export default function DataTimeSelection({ selectedDataTime, setSelectedDataTime, selectedRadarSite, historicalSelection, setHistoricalSelection, onHistoricalLoad, radarLoadStatus, radarLoadError }) {
    const [years, setYears] = useState([]);
    const [months, setMonths] = useState([]);
    const [days, setDays] = useState([]);
    const [times, setTimes] = useState([]);
    const [loadingField, setLoadingField] = useState("");
    const [optionsError, setOptionsError] = useState("");
    const isHistorical = selectedDataTime === "Historical";
    const isRadarLoading = radarLoadStatus === "downloading" || radarLoadStatus === "parsing";

    useEffect(() => {
        if (!isHistorical || !selectedRadarSite) return;
        const controller = new AbortController();
        setLoadingField("year");
        setOptionsError("");
        getAvailableYears(controller.signal)
            .then((values) => setYears([...values].sort().reverse()))
            .catch((error) => {
                if (error.name !== "AbortError") setOptionsError(error.message);
            })
            .finally(() => {
                if (!controller.signal.aborted) setLoadingField("");
            });
        return () => controller.abort();
    }, [isHistorical, selectedRadarSite]);

    useEffect(() => {
        if (!isHistorical || !historicalSelection.year) {
            setMonths([]);
            return;
        }
        const controller = new AbortController();
        setLoadingField("month");
        setOptionsError("");
        getAvailableMonths(historicalSelection.year, controller.signal)
            .then((values) => setMonths([...values].sort().reverse()))
            .catch((error) => {
                if (error.name !== "AbortError") setOptionsError(error.message);
            })
            .finally(() => {
                if (!controller.signal.aborted) setLoadingField("");
            });
        return () => controller.abort();
    }, [historicalSelection.year, isHistorical]);

    useEffect(() => {
        if (!isHistorical || !selectedRadarSite || !historicalSelection.year || !historicalSelection.month) {
            setDays([]);
            return;
        }
        const controller = new AbortController();
        setLoadingField("day");
        setOptionsError("");
        getAvailableDays(selectedRadarSite.icao, historicalSelection.year, historicalSelection.month, controller.signal)
            .then((values) => setDays([...values].sort().reverse()))
            .catch((error) => {
                if (error.name !== "AbortError") setOptionsError(error.message);
            })
            .finally(() => {
                if (!controller.signal.aborted) setLoadingField("");
            });
        return () => controller.abort();
    }, [historicalSelection.month, historicalSelection.year, isHistorical, selectedRadarSite]);

    useEffect(() => {
        if (!isHistorical || !selectedRadarSite || !historicalSelection.year || !historicalSelection.month || !historicalSelection.day) {
            setTimes([]);
            return;
        }
        const controller = new AbortController();
        setLoadingField("time");
        setOptionsError("");
        getAvailableTimes(selectedRadarSite.icao, historicalSelection.year, historicalSelection.month, historicalSelection.day, controller.signal)
            .then((values) => setTimes([...values].sort().reverse()))
            .catch((error) => {
                if (error.name !== "AbortError") setOptionsError(error.message);
            })
            .finally(() => {
                if (!controller.signal.aborted) setLoadingField("");
            });
        return () => controller.abort();
    }, [historicalSelection.day, historicalSelection.month, historicalSelection.year, isHistorical, selectedRadarSite]);

    function updateSelection(field, value) {
        setHistoricalSelection((selection) => {
            if (field === "year") return { year: value, month: "", day: "", time: "" };
            if (field === "month") return { ...selection, month: value, day: "", time: "" };
            if (field === "day") return { ...selection, day: value, time: "" };
            return { ...selection, time: value };
        });
    }

    const canLoad = selectedRadarSite && Object.values(historicalSelection).every(Boolean) && !isRadarLoading;

    return (
        <div className={`selection-card ${isHistorical ? "selection-card--historical" : ""}`}>
            <div className="selection-card__label">
                Data Time
            </div>
            <select
                value={selectedDataTime}
                onChange={(e) => setSelectedDataTime(e.target.value)}
                className="selection-card__select"
            >
                <option value="Latest">Latest</option>
                <option value="Historical">Historical</option>
            </select>
            {isHistorical && (
                <div className="historical-selection">
                    <div className="historical-selection__station">
                        {selectedRadarSite ? `${selectedRadarSite.icao} — ${selectedRadarSite.city}` : "Select a radar site on the map"}
                    </div>
                    <div className="historical-selection__fields">
                        <select value={historicalSelection.year} onChange={(e) => updateSelection("year", e.target.value)} className="selection-card__select" disabled={!selectedRadarSite || loadingField === "year"}>
                            <option value="">{loadingField === "year" ? "Loading years…" : "Year"}</option>
                            {years.map((year) => <option key={year} value={year}>{year}</option>)}
                        </select>
                        <select value={historicalSelection.month} onChange={(e) => updateSelection("month", e.target.value)} className="selection-card__select" disabled={!historicalSelection.year || loadingField === "month"}>
                            <option value="">{loadingField === "month" ? "Loading months…" : "Month"}</option>
                            {months.map((month) => <option key={month} value={month}>{month}</option>)}
                        </select>
                        <select value={historicalSelection.day} onChange={(e) => updateSelection("day", e.target.value)} className="selection-card__select" disabled={!historicalSelection.month || loadingField === "day"}>
                            <option value="">{loadingField === "day" ? "Loading days…" : "Day"}</option>
                            {days.map((day) => <option key={day} value={day}>{day}</option>)}
                        </select>
                        <select value={historicalSelection.time} onChange={(e) => updateSelection("time", e.target.value)} className="selection-card__select" disabled={!historicalSelection.day || loadingField === "time"}>
                            <option value="">{loadingField === "time" ? "Loading times…" : "Time (UTC)"}</option>
                            {times.map((time) => <option key={time} value={time}>{`${time.slice(0, 2)}:${time.slice(2, 4)}:${time.slice(4, 6)} UTC`}</option>)}
                        </select>
                    </div>
                    <button type="button" className="historical-selection__load" onClick={onHistoricalLoad} disabled={!canLoad}>
                        {isRadarLoading ? "Loading…" : "Load scan"}
                    </button>
                </div>
            )}
            {(optionsError || radarLoadError) && <div className="selection-card__error">{optionsError || radarLoadError}</div>}
            {!isHistorical && isRadarLoading && <div className="selection-card__status">Loading latest scan…</div>}
            {radarLoadStatus === "success" && <div className="selection-card__status">Radar scan loaded</div>}
        </div>
    );
}
