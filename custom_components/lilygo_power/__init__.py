"""LilyGo Power Meter - the device announces itself, Home Assistant offers it.

The board advertises `_lilygopower._tcp` over mDNS with its identity in the TXT
records, so it shows up under Settings -> Devices & Services -> Discovered with a
single button. No broker and no YAML file are involved: this integration is the
piece that turns the announcement into entities.
"""

from __future__ import annotations

import logging
from datetime import timedelta

import aiohttp
from homeassistant.config_entries import ConfigEntry
from homeassistant.const import CONF_HOST
from homeassistant.core import HomeAssistant
from homeassistant.helpers.aiohttp_client import async_get_clientsession
from homeassistant.helpers.update_coordinator import DataUpdateCoordinator, UpdateFailed

from .const import API_PATH, DEFAULT_SCAN_INTERVAL, DOMAIN

_LOGGER = logging.getLogger(__name__)

PLATFORMS = ["sensor", "binary_sensor"]


class LilygoPowerCoordinator(DataUpdateCoordinator[dict]):
    """Polls the device's JSON endpoint."""

    def __init__(self, hass: HomeAssistant, host: str) -> None:
        super().__init__(
            hass,
            _LOGGER,
            name=DOMAIN,
            update_interval=timedelta(seconds=DEFAULT_SCAN_INTERVAL),
        )
        self.host = host
        self.url = f"http://{host}{API_PATH}"

    async def _async_update_data(self) -> dict:
        session = async_get_clientsession(self.hass)
        try:
            async with session.get(self.url, timeout=aiohttp.ClientTimeout(total=8)) as response:
                if response.status != 200:
                    raise UpdateFailed(f"{self.url} answered {response.status}")
                return await response.json(content_type=None)
        except aiohttp.ClientError as err:
            raise UpdateFailed(f"cannot reach {self.url}: {err}") from err


async def async_setup_entry(hass: HomeAssistant, entry: ConfigEntry) -> bool:
    coordinator = LilygoPowerCoordinator(hass, entry.data[CONF_HOST])
    await coordinator.async_config_entry_first_refresh()
    entry.runtime_data = coordinator
    await hass.config_entries.async_forward_entry_setups(entry, PLATFORMS)

    # The device can change its address (DHCP); keep the entry pointing at it.
    entry.async_on_unload(entry.add_update_listener(_async_update_listener))
    return True


async def async_unload_entry(hass: HomeAssistant, entry: ConfigEntry) -> bool:
    return await hass.config_entries.async_unload_platforms(entry, PLATFORMS)


async def _async_update_listener(hass: HomeAssistant, entry: ConfigEntry) -> None:
    await hass.config_entries.async_reload(entry.entry_id)
