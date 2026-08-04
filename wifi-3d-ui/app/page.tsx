"use client";
import { useEffect, useState, useRef } from 'react';
import io from 'socket.io-client';
import { Canvas, useFrame } from '@react-three/fiber';
import { OrbitControls } from '@react-three/drei';

// අපගේ Node.js Data Bridge එකට සම්බන්ධ වීම
const socket = io('http://localhost:3001');

function SurfacePlot({ dataHistory }) {
  const meshRef = useRef();

  useFrame(() => {
    if (!meshRef.current || dataHistory.length === 0) return;

    const geometry = meshRef.current.geometry;
    const positions = geometry.attributes.position.array;

    // දත්ත Buffer එකෙන් (50x64) 3D මතුපිටේ Z (උස) අගයන් වෙනස් කිරීම
    let i = 0;
    for (let y = 0; y < 50; y++) {
      for (let x = 0; x < 64; x++) {
        const zIndex = (i * 3) + 2; 
        
        // අදාළ තරංගයේ විස්තාරය (Amplitude) ලබා ගැනීම
        const amplitude = dataHistory[y] ? dataHistory[y][x] : 0;
        
        // උස ප්‍රමාණය තිරයට ගැළපෙන සේ පාලනය කිරීම ( / 5 )
        positions[zIndex] = amplitude / 5; 
        i++;
      }
    }
    // හැඩය වෙනස් වූ බව Three.js වෙත දැනුම් දීම
    geometry.attributes.position.needsUpdate = true;
  });

  return (
    <mesh ref={meshRef} rotation={[-Math.PI / 2.5, 0, 0]} position={[-15, 0, -10]}>
      {/* 64 sub-carriers x 50 history rows */}
      <planeGeometry args={[30, 20, 63, 49]} />
      <meshStandardMaterial color="#00ffcc" wireframe={true} />
    </mesh>
  );
}

export default function Home() {
  const [history, setHistory] = useState([]);

  useEffect(() => {
    socket.on('csi_data', (newArray) => {
      // අලුත් දත්ත සමඟ පැරණි දත්ත පේළි 50ක් මතක තබා ගැනීම
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
      <div style={{ position: 'absolute', top: 20, left: 20, color: '#00ffcc', zIndex: 10, fontFamily: 'sans-serif' }}>
        <h2>Wi-Fi CSI 3D Radar</h2>
        <p>Status: {history.length > 0 ? "Receiving Data Live..." : "Waiting for ESP32..."}</p>
      </div>
      
      {/* 3D අවකාශය නිර්මාණය */}
      <Canvas camera={{ position: [0, 10, 20], fov: 60 }}>
        <ambientLight intensity={0.5} />
        <pointLight position={[10, 10, 10]} />
        <SurfacePlot dataHistory={history} />
        <OrbitControls />
      </Canvas>
    </div>
  );
}