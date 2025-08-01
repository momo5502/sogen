// automatically generated by the FlatBuffers compiler, do not modify

/* eslint-disable @typescript-eslint/no-unused-vars, @typescript-eslint/no-explicit-any, @typescript-eslint/no-non-null-assertion */

import * as flatbuffers from 'flatbuffers';



export class ApplicationExit implements flatbuffers.IUnpackableObject<ApplicationExitT> {
  bb: flatbuffers.ByteBuffer|null = null;
  bb_pos = 0;
  __init(i:number, bb:flatbuffers.ByteBuffer):ApplicationExit {
  this.bb_pos = i;
  this.bb = bb;
  return this;
}

static getRootAsApplicationExit(bb:flatbuffers.ByteBuffer, obj?:ApplicationExit):ApplicationExit {
  return (obj || new ApplicationExit()).__init(bb.readInt32(bb.position()) + bb.position(), bb);
}

static getSizePrefixedRootAsApplicationExit(bb:flatbuffers.ByteBuffer, obj?:ApplicationExit):ApplicationExit {
  bb.setPosition(bb.position() + flatbuffers.SIZE_PREFIX_LENGTH);
  return (obj || new ApplicationExit()).__init(bb.readInt32(bb.position()) + bb.position(), bb);
}

exitStatus():number|null {
  const offset = this.bb!.__offset(this.bb_pos, 4);
  return offset ? this.bb!.readUint32(this.bb_pos + offset) : null;
}

mutate_exit_status(value:number):boolean {
  const offset = this.bb!.__offset(this.bb_pos, 4);

  if (offset === 0) {
    return false;
  }

  this.bb!.writeUint32(this.bb_pos + offset, value);
  return true;
}

static startApplicationExit(builder:flatbuffers.Builder) {
  builder.startObject(1);
}

static addExitStatus(builder:flatbuffers.Builder, exitStatus:number) {
  builder.addFieldInt32(0, exitStatus, null);
}

static endApplicationExit(builder:flatbuffers.Builder):flatbuffers.Offset {
  const offset = builder.endObject();
  return offset;
}

static createApplicationExit(builder:flatbuffers.Builder, exitStatus:number|null):flatbuffers.Offset {
  ApplicationExit.startApplicationExit(builder);
  if (exitStatus !== null)
    ApplicationExit.addExitStatus(builder, exitStatus);
  return ApplicationExit.endApplicationExit(builder);
}

unpack(): ApplicationExitT {
  return new ApplicationExitT(
    this.exitStatus()
  );
}


unpackTo(_o: ApplicationExitT): void {
  _o.exitStatus = this.exitStatus();
}
}

export class ApplicationExitT implements flatbuffers.IGeneratedObject {
constructor(
  public exitStatus: number|null = null
){}


pack(builder:flatbuffers.Builder): flatbuffers.Offset {
  return ApplicationExit.createApplicationExit(builder,
    this.exitStatus
  );
}
}
