"""Binary sensors: link up and reading valid."""

from __future__ import annotations

from homeassistant.components.binary_sensor import BinarySensorEntity
from homeassistant.config_entries import ConfigEntry
from homeassistant.core import HomeAssistant
from homeassistant.helpers.device_registry import DeviceInfo
from homeassistant.helpers.entity_platform import AddEntitiesCallback
from homeassistant.helpers.update_coordinator import CoordinatorEntity

from .const import BINARY_SENSORS, DOMAIN, MANUFACTURER, MODEL, EntitySpec, dig


async def async_setup_entry(
    hass: HomeAssistant, entry: ConfigEntry, async_add_entities: AddEntitiesCallback
) -> None:
    coordinator = entry.runtime_data
    async_add_entities(LilygoPowerBinarySensor(coordinator, entry, spec) for spec in BINARY_SENSORS)


class LilygoPowerBinarySensor(CoordinatorEntity, BinarySensorEntity):
    _attr_has_entity_name = True
    _attr_should_poll = False

    def __init__(self, coordinator, entry: ConfigEntry, spec: EntitySpec) -> None:
        super().__init__(coordinator)
        self._spec = spec
        self._attr_unique_id = f"{entry.unique_id or entry.entry_id}_{spec.key}"
        self._attr_name = spec.name
        self._attr_device_class = spec.device_class

    @property
    def device_info(self) -> DeviceInfo:
        data = self.coordinator.data or {}
        device = data.get("device", {})
        host = self.coordinator.host
        return DeviceInfo(
            identifiers={(DOMAIN, self.coordinator.config_entry.unique_id or host)},
            name=device.get("name") or self.coordinator.config_entry.title,
            manufacturer=MANUFACTURER,
            model=MODEL,
            sw_version=device.get("fw"),
            configuration_url=f"http://{host}/",
        )

    @property
    def available(self) -> bool:
        if not super().available:
            return False
        return not (self.coordinator.data or {}).get("meter", {}).get("simulating", False)

    @property
    def is_on(self) -> bool | None:
        value = dig(self.coordinator.data, self._spec.path)
        if value is None:
            return None
        return bool(value)
