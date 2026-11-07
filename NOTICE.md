# Source attribution

The Chapter Player ESP32 firmware combines new ESP32 hardware and interface work with code derived from other projects:

- Audiobookshelf networking and parts of the embedded audio/playback implementation build on [Doug Greer's ABSpresso fork](https://github.com/douggreer/ABSpresso), which is a fork of [Tome Kent's ABSpresso](https://github.com/tomekent/ABSpresso).
- Chapter-specific browsing, controls, and portal behavior are ported from [Doug Greer's Chapter Player](https://github.com/douggreer/chapter), based on [Chris Mtopher's original Chapter project](https://github.com/chrismtopher/chapter).
- ESP-IDF and managed components are maintained by their respective authors; see `main/idf_component.yml` and `dependencies.lock` for the component list.

The ABSpresso repositories currently declare no repository license. This project therefore does not add a license that would purport to relicense inherited code. The original source authors retain their rights; attribution here does not replace permission where permission is required.
