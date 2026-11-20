"""Sensors: one per entry in the shared spec table."""

from __future__ import annotations

from homeassistant.components.sensor import SensorEntity
from homeassistant.config_entries import ConfigEntry
from homeassistant.core import HomeAssistant
from homeassistant.helpers.entity_platform import AddEntitiesCallback

from .const import SENSORS, EntitySpec, dig
from .entity import LilygoPowerEntity


async def async_setup_entry(
    hass: HomeAssistant, entry: ConfigEntry, async_add_entities: AddEntitiesCallback
) -> None:
    coordinator = entry.runtime_data
    async_add_entities(LilygoPowerSensor(coordinator, entry, spec) for spec in SENSORS)


class LilygoPowerSensor(LilygoPowerEntity, SensorEntity):
    """A single value out of the device's JSON, refreshed by the coordinator."""

    def __init__(self, coordinator, entry: ConfigEntry, spec: EntitySpec) -> None:
        super().__init__(coordinator, entry, spec)
        self._attr_native_unit_of_measurement = spec.unit
        self._attr_state_class = spec.state_class
        if spec.suggested_precision is not None:
            self._attr_suggested_display_precision = spec.suggested_precision

    @property
    def native_value(self):
        return dig(self.coordinator.data, self._spec.path)
