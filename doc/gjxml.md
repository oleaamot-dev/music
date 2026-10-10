# GarageJam XML (GJXML) — project format as written by GarageJam 8.0.1

A `.gjxml` file is UTF-8 XML with MIME type `application/x-gjxml`.

```xml
<?xml version="1.0" encoding="UTF-8"?>
<garagejam version="8.0.1">
  <project title="Sunday Jam" rate="48000" channels="2" bpm="120.000">
    <track name="Drums" file="drums-take1.wav"
           volume="1.000" pan="0.000" mute="false" solo="false"/>
  </project>
</garagejam>
```

## `<garagejam>`
Root element. `version` is the version of the writer.

## `<project>`
| attribute  | type    | default        | range          |
|------------|---------|----------------|----------------|
| `title`    | string  | `Untitled Jam` |                |
| `rate`     | integer | 48000          | 8000 – 192000  |
| `channels` | integer | 2              | 1 – 2          |
| `bpm`      | decimal | 120            | 20 – 400       |

## `<track>`
One per track, in display order.

| attribute | type    | default | meaning |
|-----------|---------|---------|---------|
| `name`    | string  | `Track` | display name |
| `file`    | path    | empty   | audio file; relative paths resolve against the folder containing the .gjxml |
| `volume`  | decimal | 1.0     | linear gain 0 – 2 |
| `pan`     | decimal | 0.0     | −1 (left) – 1 (right) |
| `mute`    | boolean | false   | `true`/`false` (`1` also accepted) |
| `solo`    | boolean | false   | when any track is soloed, only soloed tracks are heard |

## Rules for readers
* Decimal numbers always use `.` as the decimal mark, whatever the locale.
* Out-of-range values are clamped.
* Unknown elements and attributes are ignored, so newer files open in
  older versions.
