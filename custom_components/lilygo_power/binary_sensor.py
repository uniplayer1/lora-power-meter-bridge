"""Binary sensors: link up and reading valid."""

from __future__ import annotations

from homeassistant.components.binary_sensor import BinarySensorEntity
from homeassistant.config_entries import ConfigEntry
from homeassistant.core import HomeAssistant
from homeassistant.helpers.entity_platform import AddEntitiesCallback

from .const import BINARY_SENSORS, dig
from .entity import LilygoPowerEntity


async def async_setup_entry(
    hass: HomeAssistant, entry: ConfigEntry, async_add_entities: AddEntitiesCallback
) -> None:
    coordinator = entry.runtime_data
    async_add_entities(LilygoPowerBinarySensor(coordinator, entry, spec) for spec in BINARY_SENSORS)


class LilygoPowerBinarySensor(LilygoPowerEntity, BinarySensorEntity):
    @property
    def is_on(self) -> bool | None:
        value = dig(self.coordinator.data, self._spec.path)
        if value is None:
            return None
        return bool(value)
