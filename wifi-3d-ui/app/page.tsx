"use client";
import { useEffect, useState, useRef } from 'react';
import io from 'socket.io-client';
import { Canvas, useFrame } from '@react-three/fiber';
import { OrbitControls } from '@react-three/drei';

// Connect to our Node.js Data Bridge
const socket = io('http://localhost:3001');

function SurfacePlot({ dataHistory }) {
  const meshRef = useRef();

  useFrame(() => {
    if (!meshRef.current || dataHistory.length === 0) return;

    const geometry = meshRef.current.geometry;
    const positions = geometry.attributes.position.array;

    // Update the Z (height) values of the 3D surface using the data buffer (50x64)
    let i = 0;
    for (let y = 0; y < 50; y++) {
      for (let x = 0; x < 64; x++) {
        const zIndex = (i * 3) + 2;

        // Get the amplitude value of the corresponding sub-carrier
        const amplitude = dataHistory[y] ? dataHistory[y][x] : 0;

        // Scale the height value to fit the visualization
        positions[zIndex] = amplitude / 5;
        i++;
      }
    }

    // Notify Three.js that the geometry has been updated
    geometry.attributes.position.needsUpdate = true;
  });

  return (
    <mesh ref={meshRef} rotation={[-Math.PI / 2.5, 0, 0]} position={[-15, 0, -10]}>
      {/* 64 sub-carriers × 50 history rows */}
      <planeGeometry args={[30, 20, 63, 49]} />
      <meshStandardMaterial color="#00ffcc" wireframe={true} />
    </mesh>
  );
}

export default function Home() {
  const [history, setHistory] = useState([]);

  useEffect(() => {
    socket.on('csi_data', (newArray) => {
      // Store the latest 50 rows of received data
      setHistory((prev) => {
        const newHistory = [newArray, ...prev];
        if (newHistory.length > 50) newHistory.pop();
        return newHistory;
      });
    });

    return () => socket.off('csi_data');
  }, []);

  return (
    <div style={{ width: '100vw', height: '100vh', backgroundColor: '#111' }}>
      <div
        style={{
          position: 'absolute',
          top: 20,
          left: 20,
          color: '#00ffcc',
          zIndex: 10,
          fontFamily: 'sans-serif'
        }}
      >
        <h2>Wi-Fi CSI 3D Radar</h2>
        <p>Status: {history.length > 0 ? "Receiving Data Live..." : "Waiting for ESP32..."}</p>
      </div>

      {/* Create the 3D scene */}
      <Canvas camera={{ position: [0, 10, 20], fov: 60 }}>
        <ambientLight intensity={0.5} />
        <pointLight position={[10, 10, 10]} />
        <SurfacePlot dataHistory={history} />
        <OrbitControls />
      </Canvas>
    </div>
  );
}