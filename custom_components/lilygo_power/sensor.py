"""Sensors: one per entry in the shared spec table."""

from __future__ import annotations

from homeassistant.components.sensor import SensorEntity
from homeassistant.config_entries import ConfigEntry
from homeassistant.const import CONF_HOST
from homeassistant.core import HomeAssistant
from homeassistant.helpers.device_registry import DeviceInfo
from homeassistant.helpers.entity_platform import AddEntitiesCallback
from homeassistant.helpers.update_coordinator import CoordinatorEntity

from .const import DOMAIN, MANUFACTURER, MODEL, SENSORS, EntitySpec, dig


async def async_setup_entry(
    hass: HomeAssistant, entry: ConfigEntry, async_add_entities: AddEntitiesCallback
) -> None:
    coordinator = entry.runtime_data
    async_add_entities(LilygoPowerSensor(coordinator, entry, spec) for spec in SENSORS)


class LilygoPowerSensor(CoordinatorEntity, SensorEntity):
    """A single value out of the device's JSON, refreshed by the coordinator."""

    _attr_has_entity_name = True
    _attr_should_poll = False

    def __init__(self, coordinator, entry: ConfigEntry, spec: EntitySpec) -> None:
        super().__init__(coordinator)
        self._spec = spec
        self._attr_unique_id = f"{entry.unique_id or entry.entry_id}_{spec.key}"
        self._attr_name = spec.name
        self._attr_native_unit_of_measurement = spec.unit
        self._attr_device_class = spec.device_class
        self._attr_state_class = spec.state_class
        if spec.suggested_precision is not None:
            self._attr_suggested_display_precision = spec.suggested_precision

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
        # The device has a built-in simulator that produces plausible readings;
        # they must never reach the energy dashboard or long-term statistics.
        return not (self.coordinator.data or {}).get("meter", {}).get("simulating", False)

    @property
    def native_value(self):
        return dig(self.coordinator.data, self._spec.path)
