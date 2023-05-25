from pathlib import Path
import capnp
from mcap.writer import Writer
from mcap.well_known import SchemaEncoding, MessageEncoding
import argparse
import warnings

event_capnp = capnp.load("log.capnp")
event_channels = event_capnp.Event.schema.union_fields

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("rlog", help="The input rlog file to read")
    parser.add_argument(
        "--output", "-o", default="out.mcap", help="The MCAP output path to write"
    )
    args = parser.parse_args()

    f = open(args.rlog, 'rb')

    events = event_capnp.Event.read_multiple(f)

    channels = {}

    with open(args.output, "wb") as f:
        writer = Writer(f)
        writer.start()
        
        # Read the schema bin file
        with open(Path(__file__).parent / "openpilot-log.bin", "rb") as f:
            schema = f.read()

        schema_id = writer.register_schema(
            name="log.capnp:Event",
            encoding="capnproto",
            data=schema,
        )

        for topic in event_channels:
            channels[topic] = writer.register_channel(
                topic=topic,
                message_encoding="capnproto",
                schema_id=schema_id,
            )
    
        for event in events:
            channel_id = None
            for channel in channels:
                if event._has(channel):
                    channel_id = channels[channel]
                    break
            if channel_id is not None:
                writer.add_message(
                    channel_id,
                    log_time=int(event.logMonoTime),
                    data=event.as_builder().to_bytes(),
                    publish_time=int(event.logMonoTime),
                )
            else:
                warnings.warn("Empty Event message")

        writer.finish()


if __name__ == "__main__":
    main()