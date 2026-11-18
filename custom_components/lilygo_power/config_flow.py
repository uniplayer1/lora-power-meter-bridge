"""Config flow: discovery over mDNS, with a manual fallback.

The device advertises `_lilygopower._tcp` with its MAC, firmware and role in the
TXT records, so this flow normally runs itself: Home Assistant shows the device
under "Discovered", the owner presses Add, and it is configured.
"""

from __future__ import annotations

import voluptuous as vol
from homeassistant.config_entries import ConfigFlow, ConfigFlowResult
from homeassistant.const import CONF_HOST

# Requires Home Assistant 2024.4 or newer. On older versions the import moves
# to homeassistant.components.zeroconf.
from homeassistant.helpers.service_info.zeroconf import ZeroconfServiceInfo

from .const import DOMAIN


class LilygoPowerConfigFlow(ConfigFlow, domain=DOMAIN):
    """Handles the discovered device and the manual entry."""

    VERSION = 1

    def __init__(self) -> None:
        self._host: str = ""
        self._name: str = "Power Meter"
        self._model: str = ""
        self._fw: str = ""

    async def async_step_zeroconf(self, discovery_info: ZeroconfServiceInfo) -> ConfigFlowResult:
        """Called when the board's mDNS announcement is seen."""
        properties = discovery_info.properties or {}
        host = discovery_info.host
        unique_id = properties.get("mac") or host

        await self.async_set_unique_id(unique_id)
        self._abort_if_unique_id_configured(updates={CONF_HOST: host})

        self._host = host
        self._name = discovery_info.name or "Power Meter"
        self._model = properties.get("model", "")
        self._fw = properties.get("fw", "")
        self.context["title_placeholders"] = {"name": self._name, "host": host}
        return await self.async_step_confirm()

    async def async_step_confirm(self, user_input: dict | None = None) -> ConfigFlowResult:
        """One button: the address came from the announcement itself."""
        if user_input is not None:
            return self.async_create_entry(
                title=self._name,
                data={CONF_HOST: self._host, "name": self._name, "fw": self._fw},
            )
        return self.async_show_form(
            step_id="confirm",
            description_placeholders={"name": self._name, "host": self._host, "model": self._model},
        )

    async def async_step_user(self, user_input: dict | None = None) -> ConfigFlowResult:
        """Manual fallback, for when mDNS does not cross the network."""
        if user_input is not None:
            host = user_input[CONF_HOST].strip()
            await self.async_set_unique_id(host, raise_on_progress=False)
            self._abort_if_unique_id_configured(updates={CONF_HOST: host})
            return self.async_create_entry(title=host, data={CONF_HOST: host})

        return self.async_show_form(
            step_id="user",
            data_schema=vol.Schema({vol.Required(CONF_HOST): str}),
            description_placeholders={"hint": "Address of the gateway board"},
        )
