"""Compare X4 camera orientation with the synthetic head orientation for combined poses."""
import itertools, math, sys, time
import numpy as np
sys.path.insert(0, __import__('os').path.dirname(__file__))
import vr_calibrate as v

def R_head(yaw, pitch, roll):  # OpenVR: T*Ry*Rx*Rz as in freetrack_client synthetic_pose
    y, p, r = map(math.radians, (yaw, pitch, roll))
    Ry = np.array([[math.cos(y),0,math.sin(y)],[0,1,0],[-math.sin(y),0,math.cos(y)]])
    Rx = np.array([[1,0,0],[0,math.cos(p),-math.sin(p)],[0,math.sin(p),math.cos(p)]])
    Rz = np.array([[math.cos(r),-math.sin(r),0],[math.sin(r),math.cos(r),0],[0,0,1]])
    return Ry@Rx@Rz

def angle(R):
    return math.degrees(math.acos(max(-1,min(1,(np.trace(R)-1)/2))))

def camera_R(cam):
    fr = cam.frames(0.4)
    return fr[-1][:3,:3]

def main():
    pid = int(sys.argv[1]); cam = v.Camera(pid)
    M = np.diag([1.0, 1.0, -1.0])  # OpenVR head axes (x right, y up, z back) -> X4 camera (x right, y up, z forward)
    v.synth(stereo=0); time.sleep(1.5); R0 = camera_R(cam)
    poses = [(30,0,0),(0,20,0),(0,0,15),(30,20,0),(30,0,15),(0,20,15),(30,20,15),(-40,-15,-20),(60,0,30)]
    for yaw,pitch,roll in poses:
        v.synth(base=(0,0,0,yaw,pitch,roll), stereo=0); time.sleep(1.2)
        Rrel = R0.T@camera_R(cam)
        expect = M@R_head(yaw,pitch,roll)@M
        err = angle(Rrel.T@expect)
        # try alternative Euler orders to diagnose
        alts = {}
        for order in itertools.permutations('YXZ'):
            mats = {'Y': R_head(yaw,0,0), 'X': R_head(0,pitch,0), 'Z': R_head(0,0,roll)}
            Rh = mats[order[0]]@mats[order[1]]@mats[order[2]]
            alts[''.join(order)] = angle(Rrel.T@(M@Rh@M))
        best = min(alts, key=alts.get)
        print(f'pose y{yaw:+4d} p{pitch:+4d} r{roll:+4d}: error vs Ry*Rx*Rz {err:6.2f} deg | best order {best} {alts[best]:.2f} | total rot {angle(Rrel):.1f}')
    v.write_settings()

if __name__ == '__main__':
    main()
