"""Shared base for the integration's entities: which device, and when available."""

from __future__ import annotations

from homeassistant.config_entries import ConfigEntry
from homeassistant.const import EntityCategory
from homeassistant.helpers.device_registry import DeviceInfo
from homeassistant.helpers.update_coordinator import CoordinatorEntity

from .const import (
    DOMAIN,
    MANUFACTURER,
    MODEL,
    NODE_DEVICE_NAME,
    NODE_MODEL,
    OWNER_NODE,
    EntitySpec,
    dig,
)


class LilygoPowerEntity(CoordinatorEntity):
    """One value out of the gateway's JSON, on the device it describes."""

    _attr_has_entity_name = True
    _attr_should_poll = False

    def __init__(self, coordinator, entry: ConfigEntry, spec: EntitySpec) -> None:
        super().__init__(coordinator)
        self._spec = spec
        self._attr_unique_id = f"{entry.unique_id or entry.entry_id}_{spec.key}"
        self._attr_name = spec.name
        self._attr_device_class = spec.device_class
        if spec.diagnostic:
            self._attr_entity_category = EntityCategory.DIAGNOSTIC

    @property
    def device_info(self) -> DeviceInfo:
        entry = self.coordinator.config_entry
        gateway_id = entry.unique_id or self.coordinator.host
        if self._spec.owner == OWNER_NODE:
            # The meter node is reached through the gateway and identified
            # through it, so it stays the same device when the node is replaced.
            return DeviceInfo(
                identifiers={(DOMAIN, f"{gateway_id}_keller")},
                name=NODE_DEVICE_NAME,
                manufacturer=MANUFACTURER,
                model=NODE_MODEL,
                via_device=(DOMAIN, gateway_id),
            )
        data = self.coordinator.data or {}
        device = data.get("device", {})
        host = self.coordinator.host
        return DeviceInfo(
            identifiers={(DOMAIN, gateway_id)},
            name=device.get("name") or entry.title,
            manufacturer=MANUFACTURER,
            model=MODEL,
            sw_version=device.get("fw"),
            configuration_url=f"http://{host}/",
        )

    @property
    def available(self) -> bool:
        if not super().available:
            return False
        data = self.coordinator.data or {}
        if self._spec.requires is not None and dig(data, self._spec.requires) is None:
            return False
        # The device has a built-in simulator that produces plausible readings;
        # they must never reach the energy dashboard or long-term statistics.
        return not data.get("meter", {}).get("simulating", False)
