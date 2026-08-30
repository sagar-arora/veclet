package toolchain_test

import (
	"testing"

	storagev1 "github.com/sagar-arora/veclet/gen/go/veclet/storage/v1"
	vecletv1 "github.com/sagar-arora/veclet/gen/go/veclet/v1"
	"google.golang.org/protobuf/proto"
)

func TestGeneratedContractsRoundTripCanonicalStoredRecord(t *testing.T) {
	stored := &storagev1.StoredRecord{
		LocalIndexId: 7,
		Record: &vecletv1.VectorRecord{
			VectorId:    "customer-42",
			Version:     1,
			Embedding:   []float32{0.25, -0.5},
			PayloadData: "opaque payload",
		},
	}

	encoded, err := proto.Marshal(stored)
	if err != nil {
		t.Fatalf("marshal StoredRecord: %v", err)
	}

	decoded := new(storagev1.StoredRecord)
	if err := proto.Unmarshal(encoded, decoded); err != nil {
		t.Fatalf("unmarshal StoredRecord: %v", err)
	}
	if got, want := decoded.GetRecord().GetVectorId(), "customer-42"; got != want {
		t.Fatalf("vector ID = %q, want %q", got, want)
	}
	if got, want := len(decoded.GetRecord().GetEmbedding()), 2; got != want {
		t.Fatalf("embedding length = %d, want %d", got, want)
	}
}

func TestGeneratedContractsExposeExpectedServices(t *testing.T) {
	if got, want := vecletv1.QueryService_ServiceDesc.ServiceName, "veclet.v1.QueryService"; got != want {
		t.Fatalf("query service = %q, want %q", got, want)
	}
	if got, want := vecletv1.DataService_ServiceDesc.ServiceName, "veclet.v1.DataService"; got != want {
		t.Fatalf("data service = %q, want %q", got, want)
	}
}
